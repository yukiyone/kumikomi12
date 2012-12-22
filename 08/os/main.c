#include "defines.h"
#include "kozos.h"
#include "interrupt.h"
#include "lib.h"

/*システムタスクとユーザスレッドの起動*/
static int start_threads(int argc, char *argv[])
{
  /*コマンド処理スレッドの起動*/
  kz_run(test08_1_main, "command", 0x100, 0, NULL);
  return 0;
}

int main(void)
{
  INTR_DISABLE;

  puts("kozos boot succeed!\n");
  kz_start(start_threads, "start", 0x100, 0, NULL);
  return 0;
}
