// Interactive MicroPython REPL on the SNES (build/mpyrepl.sfc). The whole
// engine — dual mailbox/console I/O, on-target compile-and-run, the
// interactive loop with the raw mode used by the upstream-test runner,
// and the runtime hooks — lives in port/pyexec.c, shared with the
// workstation ROM (mpyos). This main just brings the machine up, runs the
// loop, and exits PASS on ^D so scripted sessions terminate cleanly.

#include "py/cstack.h"
#include "py/gc.h"
#include "py/runtime.h"

#include "pyexec.h"

#include "../snes/console.h"
#include "../snes/mailbox.h"
#include "../snes/oskb.h"

// GC heap: WRAM bank $7F below the mailbox, one bank only (far pointer
// arithmetic must never cross a bank boundary, DECISIONS.md). 56KB.
#define HEAP_START 0x7F0000UL
#define HEAP_END 0x7FE000UL

// C stack: $0100-$1FFF in bank 0 (see snes/linker.scm)
#define STACK_SIZE 0x1D00

int main(void)
{
  int stack_dummy;
  mb_init();
  mp_cstack_init_with_top(&stack_dummy, STACK_SIZE);
  gc_init((void *)HEAP_START, (void *)HEAP_END);
  console_init();
  oskb_init();
  mp_init();
  // No joypad exit (there is no "after" on a console). The preload leaves
  // a demo block pending at the continuation prompt: Start runs it.
  // NB: each echoed line must fit CON_TEXT_COLS (31) or the console wraps.
  pyexec_repl(0, "for i in range(3):\n  print(f\"hello SNES {i}\")\n");
  mp_deinit();
  mb_finish(MB_STATUS_PASS);
  return 0;
}
