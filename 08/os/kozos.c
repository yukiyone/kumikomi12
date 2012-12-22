#include "defines.h"
#include "kozos.h"
#include "intr.h"
#include "interrupt.h"
#include "syscall.h"
#include "lib.h"

#define THREAD_NUM 6 /*TCBの個数*/
#define THREAD_NAME_SIZE 15 /*スレッド名の最大長*/

typedef struct _kz_context{
  uint32 sp;
}kz_context;

typedef struct _kz_thread{
  struct _kz_thread *next; /*レディーキューへの接続に利用するnextポインタ*/
  char name[THREAD_NAME_SIZE + 1]; /*スレッドの名前*/
  char *stack; /*スレッドのスタック*/
  
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
  char dummy[16];
} kz_thread;

/*スレッドのレディーキュー*/
static struct{
  kz_thread *head; /*レディーキューの先頭のエントリ*/
  kz_thread *tail; /*レディーキューの末尾のエントリ*/
}readyque;

static kz_thread *current; /*カレント・スレッド*/
static kz_thread threads[THREAD_NUM]; /*タスク・コントロール・ブロック*/
static kz_handler_t handlers[SOFTVEC_TYPE_NUM]; /*割り込みハンドラ|OSが管理する割り込みハンドラ*/

void dispatch(kz_context *context);

/*カレント・スレッドをレディーキューから抜き出す*/
static int getcurrent(void)
{  
  if(current == NULL){
    return -1;
  }

  /*カレント／スレッドは必ず先頭にあるはずなので、先頭から抜き出す*/
  readyque.head = current->next;
  if(readyque.head == NULL){
    readyque.tail = NULL;
  }
  
  current->next = NULL;
  return 0;
}

/*カレント・スレッドをレディーキューにつなげる*/
static int putcurrent(void)
{
  if(current == NULL){
    return -1;
  }

  if(readyque.tail){
    readyque.tail->next = current;    
  }else{
    readyque.head = current;
  }
  readyque.tail = current;

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
static kz_thread_id_t thread_run(kz_func_t func, char *name, int stacksize, int argc, char *argv[])
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
  
  /*プログラム・カウンタ*/
  *(--sp) = (uint32)thread_init;
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

static void thread_intr(softvec_type_t type, unsigned long sp);

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
			       p->un.run.stacksize,
			       p->un.run.argc, p->un.run.argv);
    break;
  case KZ_SYSCALL_TYPE_EXIT:
    thread_exit();
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
  if(!readyque.head)
    kz_sysdown();

  current = readyque.head;
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

void kz_start(kz_func_t func, char *name, int stacksize, int argc, char *argv[])
{
  /*
    意向で呼び出すスレッド関連のライブラリ関数内部でcurrentを
    見ている場合があるので、currentをNULLに初期化しておく
   */
  current = NULL;
  readyque.head = readyque.tail = NULL;
  memset(threads, 0, sizeof(threads));
  memset(handlers, 0, sizeof(handlers));

  /*割り込みハンドラの登録*/
  setintr(SOFTVEC_TYPE_SYSCALL, syscall_intr);
  setintr(SOFTVEC_TYPE_SOFTERR, softerr_intr);

  /*システム・コール発行付加なので直接関数を呼び出してスレッド作成する*/
  current = (kz_thread *)thread_run(func, name, stacksize, argc, argv);

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
