#include "defines.h"
#include "kozos.h"
#include "interrupt.h"
#include "lib.h"

kz_thread_id_t test09_1_id;
kz_thread_id_t test09_2_id;
kz_thread_id_t test09_3_id;

/*システムタスクとユーザスレッドの起動*/
static int start_threads(int argc, char *argv[])
{
  /*コマンド処理スレッドの起動*/
  /*kz_run(test08_1_main, "command", 0x100, 0, NULL);*/
  test09_1_id = kz_run(test09_1_main, "test09_1", 1, 0x100, 0, NULL);
  test09_2_id = kz_run(test09_2_main, "test09_2", 2, 0x100, 0, NULL);
  test09_3_id = kz_run(test09_3_main, "test09_3", 3, 0x100, 0, NULL);

  kz_chpri(15);
  INTR_ENABLE;
  while(1){
    asm volatile("sleep");
  }
  return 0;
}

int main(void)
{
  INTR_DISABLE;

  puts("kozos boot succeed!\n");
  kz_start(start_threads, "idle", 0, 0x100, 0, NULL);
  return 0;
}
