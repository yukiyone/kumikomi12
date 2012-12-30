#include "defines.h"
#include "kozos.h"
#include "intr.h"
#include "interrupt.h"
#include "syscall.h"
#include "memory.h"
#include "lib.h"

#define THREAD_NUM 6 /*TCBの個数*/
#define PRIORITY_NUM 16
#define THREAD_NAME_SIZE 15 /*スレッド名の最大長*/

typedef struct _kz_context{
  uint32 sp;
}kz_context;

typedef struct _kz_thread{
  struct _kz_thread *next; /*レディーキューへの接続に利用するnextポインタ*/
  char name[THREAD_NAME_SIZE + 1]; /*スレッドの名前*/
  int priority;
  char *stack; /*スレッドのスタック*/
  uint32 flags;
#define KZ_THREAD_FLAG_READY (1 << 0)

  struct { /*スレッドのスタートアップに渡すパラメータ*/
    kz_func_t func;
    int argc;
    char **argv;
  }init;
  
  struct { /*システムコール用のバッファ*/
    kz_syscall_type_t type;
    kz_syscall_param_t *param;
  } syscall;
  
  kz_context context; /*コンテキスト情報*/
  char dummy[8];
} kz_thread;

/*メッセージ・バッファ*/
typedef struct _kz_msgbuf{
  struct _kz_msgbox *next;
  kz_thread *sender;
  struct {
    int size;
    char *p;
  }param;
}kz_msgbuf;

/*メッセージボックス*/
typedef struct _kz_msgbox{
  kz_thread *receiver;
  kz_msgbuf *head;
  kz_msgbuf *tail;

  long dummy[1];
}kz_msgbox;

/*スレッドのレディーキュー*/
static struct{
  kz_thread *head; /*レディーキューの先頭のエントリ*/
  kz_thread *tail; /*レディーキューの末尾のエントリ*/
}readyque[PRIORITY_NUM];

static kz_thread *current; /*カレント・スレッド*/
static kz_thread threads[THREAD_NUM]; /*タスク・コントロール・ブロック*/
static kz_handler_t handlers[SOFTVEC_TYPE_NUM]; /*割り込みハンドラ|OSが管理する割り込みハンドラ*/
static kz_msgbox msgboxes[MSGBOX_ID_NUM];

void dispatch(kz_context *context);

/*カレント・スレッドをレディーキューから抜き出す*/
static int getcurrent(void)
{  
  if(current == NULL){
    return -1;
  }

  if(!(current->flags & KZ_THREAD_FLAG_READY)){
    /*既にない場合は無視*/
    return 1;
  }

  readyque[current->priority].head = current->next;
  if(readyque[current->priority].head == NULL){
    readyque[current->priority].tail = NULL;
  }

  /*READYビットを落とす*/
  current->flags &= ~KZ_THREAD_FLAG_READY;
  current->next = NULL;
  
  return 0;
}

/*カレント・スレッドをレディーキューにつなげる*/
static int putcurrent(void)
{
  if(current == NULL){
    return -1;
  }
  if(current->flags & KZ_THREAD_FLAG_READY){
    return 1;
  }
  
  
  if(readyque[current->priority].tail){
    readyque[current->priority].tail->next = current;    
  }else{
    readyque[current->priority].head = current;
  }
  readyque[current->priority].tail = current;
  
  /*READYビットを立てる*/
  current->flags |= KZ_THREAD_FLAG_READY;

  return 0;
}

static void thread_end(void)
{
  kz_exit();
}

/*スレッドのスタートアップ*/
static void thread_init(kz_thread *thp)
{
  thp->init.func(thp->init.argc, thp->init.argv);
  thread_end();
}

/*システムコールの処理(kz_run():スレッドの起動*/
static kz_thread_id_t thread_run(kz_func_t func, char *name, int priority, int stacksize, int argc, char *argv[])
{
  int i;
  kz_thread *thp;
  uint32 *sp;
  extern char userstack;
  static char *thread_stack = &userstack; /*ユーザスタックに利用される領域*/
  
  /*空いているタスク・コントロール・ブロックを検索*/
  for(i = 0; i < THREAD_NUM; i++){
    thp = &threads[i];
    if(!thp->init.func)
      break;
  }
  if(i == THREAD_NUM)
    return -1;

  memset(thp, 0, sizeof(*thp));

  /*タスク・コントロール・ブロック*/
  strcpy(thp->name, name);
  thp->next = NULL;
  thp->priority = priority;
  thp->flags = 0;
  
  thp->init.func = func;
  thp->init.argc = argc;
  thp->init.argv = argv;
  
  /*スタック領域を獲得*/
  memset(thread_stack, 0, stacksize);
  thread_stack += stacksize;
  
  thp->stack = thread_stack; /*スタックを設定*/
  
  /*スタックの初期化*/
  sp = (uint32 *)thp->stack;
  *(--sp) = (uint32)thread_end;
  
  /*
   *プログラム・カウンタ
   *スレッドの優先度がゼロの場合には、割り込み禁止スレッドとする
   */
  *(--sp) = (uint32)thread_init| ((uint32)(priority ? 0 :0xc0) << 24) ;
  *(--sp) = 0;/*ER6*/
  *(--sp) = 0;/*ER5*/
  *(--sp) = 0;/*ER4*/
  *(--sp) = 0;/*ER3*/
  *(--sp) = 0;/*ER2*/
  *(--sp) = 0;/*ER1*/

  /*スレッドのスタートアップ(thread_init())に渡す引数*/
  *(--sp) = (uint32)thp; /*コンテキストとしてスタックポインタ尾を保存*/
  
  /*スレッドのコンテキストを設定*/
  thp->context.sp = (uint32)sp; /*スタックポインタの保存*/
  
  /*システムコールを呼び出したスレッドをレディーキューに戻す*/
  putcurrent();
  
  /*真に作成したスレッドを、レディーキューに接続する*/
  current = thp;
  putcurrent();

  return (kz_thread_id_t)current;
}

/*システム・コールの処理(kz_exit():スレッドの終了)*/
static int thread_exit(void)
{
  /*本来ならスタックも解放して再利用するべきだが省略*/
  puts(current->name);
  puts("exit");
  memset(current, 0, sizeof(*current));
  return 0;
}

static int thread_wait(void)
{
  putcurrent();
  return 0;
}

static int thread_sleep(void)
{
  return 0;
}

static int thread_wakeup(kz_thread_id_t id)
{
  /*ウェイクアップを呼び出したスレッドをレディキューに戻す*/
  putcurrent();

  /*指定されたスレッドをレディーキューに接続してウェイクアップする*/
  current = (kz_thread *)id;
  putcurrent();

  return 0;
}

static kz_thread_id_t thread_getid(void)
{
  putcurrent();
  return (kz_thread_id_t)current;
}

static int thread_chpri(int priority)
{
  int old = current->priority;
  if(priority >= 0)
    current->priority = priority;
  
  putcurrent();
  return old;
}

static void *thread_kmalloc(int size)
{
  putcurrent();
  return kzmem_alloc(size);
}

static int thread_kmfree(char *p)
{
  kzmem_free(p);
  putcurrent();
  return 0;
}


static void sendmsg(kz_msgbox *mboxp, kz_thread *thp, int size, char *p)
{
  kz_msgbuf *mp;
  
  /*メッセージのバッファ作成*/
  mp = (kz_msgbuf *) kzmem_alloc(sizeof(*mp));
  if(mp == NULL)
    kz_sysdown();

  mp->next = NULL;
  mp->sender = thp;
  mp->param.size = size;
  mp->param.p = p;

  /*メッセージボックスの末尾にメッセージを接続する*/
  if(mboxp->tail){
    mboxp->tail->next = mp;
  }else{
    mboxp->head = mp;
  }
  mboxp->tail = mp;
}
static void recvmsg(kz_msgbox *mboxp)
{
  kz_msgbuf *mp;
  kz_syscall_param_t *p;

  /*メッセージボックスの先頭にあるメッセージを抜き出す*/
  mp = mboxp->head;
  mboxp->head = mp->next;
  if(mboxp->head == NULL)
    mboxp->tail = NULL;
  mp->next = NULL;

  /*メッセージを受信するスレッドに返す値を設定する*/
  p = mboxp->receiver->syscall.param;
  p->un.recv.ret = (kz_thread_id_t)mp->sender;
  if(p->un.recv.sizep)
    *(p->un.recv.sizep) = mp->param.size;

  if(p->un.recv.pp)
    *(p->un.recv.pp) = mp->param.p;
  
  /*受信待ちスレッドはいなくなったので、NULLに戻す*/
  mboxp->receiver = NULL;

  /*メッセージバッファの解放*/
  kzmem_free(mp);
}

static void thread_intr(softvec_type_t type, unsigned long sp);


static int thread_send(kz_msgbox_id_t id, int size, char *p)
{
  kz_msgbox *mboxp = &msgboxes[id];
  
  putcurrent();
  sendmsg(mboxp, current, size, p);

  /*受信待ちスレッドが存在している場合には受信処理を行う*/
  if(mboxp->receiver){
    current = mboxp->receiver;
    recvmsg(mboxp);/*メッセージの受信処理*/
    putcurrent(); /*受信により動作可能になったので、ブロック解除する*/
  }
  return size;
}

static kz_thread_id_t thread_recv(kz_msgbox_id_t id, int *sizep, char **pp)
{
  kz_msgbox *mboxp = &msgboxes[id];
  
  if(mboxp->receiver)
    kz_sysdown();

  /*受信待ちスレッドに設定*/
  mboxp->receiver = current;

  if(mboxp->head == NULL){
    /*メッセージボックスにメッセージが無いので、スレッドをスリープさせる*/
    return -1;
  }
  recvmsg(mboxp);
  putcurrent();/*メッセージを受信できたので、レディー状態にする*/
  return current->syscall.param->un.recv.ret;
}


/*割り込みハンドラの登録*/
static int setintr(softvec_type_t type, kz_handler_t handler)
{
  /*
    割り込みを受け付けるために、ソフトウェア割り込みベクタに
    OSの割り込み処理の入り口となる関数を登録
   */
  softvec_setintr(type, thread_intr);
  handlers[type] = handler;
  return 0;
}

/*システム・コールの処理関数の呼び出し*/
static void call_functions(kz_syscall_type_t type, kz_syscall_param_t *p)
{
  switch(type){
  case KZ_SYSCALL_TYPE_RUN:
    p->un.run.ret = thread_run(p->un.run.func, p->un.run.name,
			       p->un.run.priority, p->un.run.stacksize,
			       p->un.run.argc, p->un.run.argv);
    break;
  case KZ_SYSCALL_TYPE_EXIT:
    thread_exit();
    break;
  case KZ_SYSCALL_TYPE_WAIT:
    p->un.wait.ret = thread_wait();
    break;
  case KZ_SYSCALL_TYPE_SLEEP:
    p->un.sleep.ret = thread_sleep();
    break;
  case KZ_SYSCALL_TYPE_WAKEUP:
    p->un.wakeup.ret = thread_wakeup(p->un.wakeup.id);
    break;
  case KZ_SYSCALL_TYPE_GETID:
    p->un.getid.ret = thread_getid();
    break;
  case KZ_SYSCALL_TYPE_CHPRI:
    p->un.chpri.ret = thread_chpri(p->un.chpri.priority);
    break;
  case KZ_SYSCALL_TYPE_KMALLOC:
    p->un.kmalloc.ret = thread_kmalloc(p->un.kmalloc.size);
    break;
  case KZ_SYSCALL_TYPE_KMFREE:
    p->un.kmfree.ret = thread_kmfree(p->un.kmfree.p);
    break;
  case KZ_SYSCALL_TYPE_SEND:
    p->un.send.ret = thread_send(p->un.send.id, p->un.send.size, p->un.send.p);
    break;
  case KZ_SYSCALL_TYPE_RECV:
    p->un.recv.ret = thread_recv(p->un.recv.id, p->un.recv.sizep, p->un.recv.pp);
    break;    
  default:
    break;
  }
  
}

/*システム・コールの処理*/
static void syscall_proc(kz_syscall_type_t type, kz_syscall_param_t *p)
{
  /*
   システムコールを呼び出したスレッドをレディーキューから
   外した状態で処理関数を呼び出す。このためシステム・コールを
   呼び出したスレッドをそのまま継続させたい場合には、
   処理関数の内部でputcurrent()を行う必要がある。
   */

  getcurrent();
  call_functions(type, p);
}

/*スレッドのスケジューリング*/
static void schedule(void)
{
  int i;

  /*
   *優先度の高い順(優先度の値の小さい順)にレディー・キューを見て、
   *動作可能なスレッドを検索する
   */
  for(i = 0; i<PRIORITY_NUM; i++){
    if(readyque[i].head) /*見つからなかった場合*/
      break;
  }
  if(i == PRIORITY_NUM)
    kz_sysdown();

  current = readyque[i].head;
}

/*システムコールの呼び出し*/
static void syscall_intr(void)
{
  syscall_proc(current->syscall.type, current->syscall.param);
}

static void softerr_intr(void)
{
  puts(current->name);
  puts("DOWN\n");
  getcurrent(); /*レディーキューから外す*/
  thread_exit(); /*スレッドを終了する*/
}

/*割り込み処理の入り口関数*/
static void thread_intr(softvec_type_t type, unsigned long sp)
{
  /*カレント・スレッドのコンテクストを保存*/
  current->context.sp = sp;

  /*
    割り込みごとの処理を実行する
    SOFTVEC_TYPE_SYSCALL, SOFTVEC_TYPE_SOFTERRの場合は
    syscall_intr(), softerr_intr()がハンドラに登録されているので、
    それらが実行される
   */
  if(handlers[type])
    handlers[type]();

  /*次に動作するスレッドをスケジューリング*/
  schedule();

  /*
    スレッドのディスパッチ
    スケジューリングされたスレッドをディスパッチする
   */
  dispatch(&current->context);
}

void kz_start(kz_func_t func, char *name, int priority, int stacksize, int argc, char *argv[])
{
  kzmem_init();
  
  /*
    以降で呼び出すスレッド関連のライブラリ関数内部でcurrentを
    見ている場合があるので、currentをNULLに初期化しておく
   */
  current = NULL;
  
  memset(readyque, 0, sizeof(readyque));
  memset(threads, 0, sizeof(threads));
  memset(handlers, 0, sizeof(handlers));
  memset(msgboxes, 0, sizeof(msgboxes));

  /*割り込みハンドラの登録*/
  setintr(SOFTVEC_TYPE_SYSCALL, syscall_intr);
  setintr(SOFTVEC_TYPE_SOFTERR, softerr_intr);

  /*システム・コール発行付加なので直接関数を呼び出してスレッド作成する*/
  current = (kz_thread *)thread_run(func, name, priority, stacksize, argc, argv);
  
  /*最初のスレッドを起動*/
  dispatch(&current->context);
  
}
/*OS内部で致命的なエーラが発生した場合には、この関数を呼ぶ*/
void kz_sysdown(void)
{
  puts("system error!\n");
  while(1)
    ;
}

/*システム・コール呼び出し用ライブラリ関数*/
void kz_syscall(kz_syscall_type_t type, kz_syscall_param_t *param)
{
  current->syscall.type = type;
  current->syscall.param = param;
  /*トラップ割り込みの発行*/
  asm volatile("trapa #0"); 
}
