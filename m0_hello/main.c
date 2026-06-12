#include "../snes/mailbox.h"

// M0: prove the toolchain, startup code, linker map and test harness work.
// Exercises .data initialization (initialized global) and .bss clearing on
// the way: if either is broken the message comes out wrong.
static char greeting[] = "Hello from Calypsi on SNES\n";
static int bss_probe[4];

int main(void)
{
  mb_init();
  mb_puts(greeting);
  if (bss_probe[0] | bss_probe[1] | bss_probe[2] | bss_probe[3]) {
    mb_puts("BSS not cleared\n");
    mb_finish(2);
  }
  mb_puts("M0 done\n");
  mb_finish(MB_STATUS_PASS);
  return 0;
}
