// Interactive MicroPython REPL on the SNES (build/mpyrepl.sfc): reads lines
// from the mailbox stdin ring, compiles them ON TARGET (lexer -> parser ->
// bytecode compiler, MICROPY_ENABLE_COMPILER) and executes them in the split
// VM. Input is echoed so the mailbox log reads like a terminal session.
// Ctrl-D (0x04) at the start of a line exits with MB_STATUS_PASS.
//
// The runtime hooks (gc_collect, nlr_jump_fail, stdout) are duplicated from
// port/main.c rather than shared, so the frozen-module ROMs keep their exact
// translation units.

#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"

#include "../snes/mailbox.h"

// GC heap: WRAM bank $7F below the mailbox, one bank only (far pointer
// arithmetic must never cross a bank boundary, DECISIONS.md). 56KB.
#define HEAP_START 0x7F0000UL
#define HEAP_END 0x7FE000UL

// C stack: $0100-$1FFF in bank 0 (see snes/linker.scm)
#define STACK_SIZE 0x1D00

// Compile and run one REPL input unit. MP_PARSE_SINGLE_INPUT + is_repl makes
// bare expressions print their value (via __repl_print__), like CPython.
// NB: no locals are modified between nlr_push and the exception path
// (Calypsi drops volatile stores to stack locals in setjmp functions,
// DECISIONS.md bug 2).
static void exec_line(vstr_t *line)
{
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                vstr_str(line), line->len, 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t parse_tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
    mp_obj_t module_fun = mp_compile(&parse_tree, source_name, true);
    mp_call_function_0(module_fun);
    nlr_pop();
  } else {
    mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
  }
}

// Read one logical input unit (following continuation prompts for compound
// statements) into line, echoing as we go. Returns 0 on Ctrl-D at the start
// of a line, 1 otherwise.
static int read_input(vstr_t *line)
{
  int at_line_start = 1;
  for (;;) {
    char c = mb_getc();
    if (c == 0x04 && at_line_start && line->len == 0) {
      return 0;
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      mb_putc('\n');
      if (mp_repl_continue_with_input(vstr_null_terminated_str(line))) {
        vstr_add_byte(line, '\n');
        mb_puts("... ");
        at_line_start = 1;
        continue;
      }
      return 1;
    }
    vstr_add_byte(line, c);
    mb_putc(c);
    at_line_start = 0;
  }
}

int main(void)
{
  int stack_dummy;
  mb_init();
  mp_cstack_init_with_top(&stack_dummy, STACK_SIZE);
  gc_init((void *)HEAP_START, (void *)HEAP_END);
  mp_init();
  mb_puts("MicroPython on SNES; Ctrl-D exits\n");
  vstr_t line;
  vstr_init(&line, 64);
  for (;;) {
    vstr_reset(&line);
    mb_puts(">>> ");
    if (!read_input(&line)) {
      break;
    }
    if (line.len == 0) {
      continue;
    }
    exec_line(&line);
  }
  mb_puts("\nbye\n");
  mp_deinit();
  mb_finish(MB_STATUS_PASS);
  return 0;
}

// No filesystem: execfile()/compile-from-file cannot work (same stub as
// ports/minimal)
mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
  (void)filename;
  mp_raise_OSError(MP_ENOENT);
}

// ---- runtime hooks (same as port/main.c; see comments there) ----------------

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

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  mp_hal_stdout_tx_strn(str, len);
}
