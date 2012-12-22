#include "defines.h"
#include "kozos.h"
#include "syscall.h"


/*システム・コール*/
kz_thread_id_t kz_run(kz_func_t func, char *name, int stacksize, int argc, char *argv[])
{
  /*スタックはスレッドごとに確保されるので、パラメータ域は自動変数としてスタック上に確保する*/
  kz_syscall_param_t param;

  param.un.run.func = func;
  param.un.run.name = name;
  param.un.run.stacksize = stacksize;
  param.un.run.argc = argc;
  param.un.run.argv = argv;
  /*システムコールを呼び出す*/
  kz_syscall(KZ_SYSCALL_TYPE_RUN, &param);
  /*システムコールの応答が構造体に書くほうされているので戻りあたいとして返す*/
  return param.un.run.ret; 
}

void kz_exit(void)
{
  kz_syscall(KZ_SYSCALL_TYPE_EXIT, NULL);
}
