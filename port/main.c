#include "py/bc.h"
#include "py/builtin.h"
#include "py/cstack.h"
#if defined(__VBCC__)
// mp_make_function_from_proto_fun reaches Calypsi transitively; vbcc needs it
// explicit. Guarded so the Calypsi translation unit stays byte-identical (its
// M3 pass is layout-sensitive — any size shift can flip it).
#include "py/emitglue.h"
#endif
#include "py/frozenmod.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objmodule.h"
#include "py/runtime.h"

#include "../snes/console.h"
#include "../snes/mailbox.h"

// GC heap: WRAM bank $7F below the mailbox (one bank only — far pointer
// arithmetic must never cross a bank boundary, DECISIONS.md). 56KB.
#define HEAP_START 0x7F0000UL
#define HEAP_END 0x7FE000UL

// C stack: $0100-$1FFF in bank 0 (see snes/linker.scm)
#define STACK_SIZE 0x1D00

// Execute the frozen main.py; modeled on pyexec_frozen_module but without
// dragging in the REPL. NB: no locals are modified between nlr_push and the
// exception path — Calypsi drops volatile stores to stack locals
// (DECISIONS.md, bug 2), so this function must not rely on them.
static void run_frozen(void)
{
  // Execute the frozen main.py through the regular import machinery (the
  // frozen module is registered as "main"). The earlier direct
  // proto_fun-call path miscompiled in some ROM layouts; the importer path
  // is exercised (and proven) by every frozen package import.
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_import_name(qstr_from_str("main"), mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    nlr_pop();
  } else {
    mb_puts("uncaught exception\n");
    mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    mb_finish(MB_STATUS_PANIC);
  }
}

int main(void)
{
  int stack_dummy;
  mb_init();
  mp_cstack_init_with_top(&stack_dummy, STACK_SIZE);
  gc_init((void *)HEAP_START, (void *)HEAP_END);
  // Boot console: interpreter startup plus frozen imports take ~1-2 minutes
  // of real time on a 3.58 MHz 65816, so show progress on the TV instead of
  // a black screen. snesfb.init() calls console_disable() when it takes
  // over the PPU (the VRAM layouts overlap).
  console_init();
  console_puts("MicroPython on SNES\nbooting (this takes a moment)...\n");
  console_flush();
  mp_init();
  run_frozen();
  mp_deinit();
  mb_finish(MB_STATUS_PASS);
  return 0;
}

// Conservative root scan. Calypsi keeps locals in two places: the bank-0
// hardware stack (our linker puts it in $1000-$1FFF) and direct-page pseudo
// registers ($0000-$00FF). 32-bit pointers can sit at any even offset, so
// each region is scanned twice, phase-shifted by 2 bytes.
void gc_collect(void)
{
  gc_collect_start();
  gc_collect_root((void **)0x7E0000UL, 0x100 / 4);
  gc_collect_root((void **)0x7E0002UL, (0x100 - 4) / 4);
  gc_collect_root((void **)0x7E0100UL, 0x1F00 / 4);
  gc_collect_root((void **)0x7E0102UL, (0x1F00 - 4) / 4);
  gc_collect_end();
}

// No filesystem: execfile()/compile-from-file cannot work (same stub as
// ports/minimal). Referenced since the on-target compiler was enabled.
mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
  (void)filename;
  mp_raise_OSError(MP_ENOENT);
}

// No filesystem: the importer finds only frozen modules
mp_import_stat_t mp_import_stat(const char *path)
{
  (void)path;
  return MP_IMPORT_STAT_NO_EXIST;
}

void nlr_jump_fail(void *val)
{
  (void)val;
  mb_puts("FATAL: nlr_jump_fail\n");
  mb_finish(MB_STATUS_PANIC);
  for (;;) {
  }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
  mp_uint_t n = len;
  while (len--) {
    char c = *str++;
    mb_putc(c);
    console_putc(c); // no-op once snesfb owns the PPU
    if (c == '\n') {
      console_flush();
    }
  }
  return n;
}

// "cooked" = \n becomes \r\n on real terminals; the mailbox wants plain \n
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  mp_hal_stdout_tx_strn(str, len);
}
