#include "py/bc.h"
#include "py/cstack.h"
#include "py/frozenmod.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/objmodule.h"
#include "py/runtime.h"

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
  void *frozen_data;
  int frozen_type;
  mp_find_frozen_module("main.py", &frozen_type, &frozen_data);
  if (frozen_type != MP_FROZEN_MPY) {
    mb_puts("FATAL: frozen main.py not found\n");
    mb_finish(MB_STATUS_PANIC);
  }
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    const mp_frozen_module_t *frozen = frozen_data;
    mp_module_context_t *ctx = m_new_obj(mp_module_context_t);
    ctx->module.globals = mp_globals_get();
    ctx->constants = frozen->constants;
    mp_obj_t module_fun = mp_make_function_from_proto_fun(frozen->proto_fun, ctx, NULL);
    mp_call_function_0(module_fun);
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
    mb_putc(*str++);
  }
  return n;
}

// "cooked" = \n becomes \r\n on real terminals; the mailbox wants plain \n
void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  mp_hal_stdout_tx_strn(str, len);
}
