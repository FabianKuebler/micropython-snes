// Interactive MicroPython REPL on the SNES (build/mpyrepl.sfc): reads lines
// from the mailbox stdin ring, compiles them ON TARGET (lexer -> parser ->
// bytecode compiler, MICROPY_ENABLE_COMPILER) and executes them in the split
// VM. Input is echoed so the mailbox log reads like a terminal session.
// Ctrl-D (0x04) at the start of a line exits with MB_STATUS_PASS.
//
// The runtime hooks (gc_collect, nlr_jump_fail, stdout) are duplicated from
// port/main.c rather than shared, so the frozen-module ROMs keep their exact
// translation units.

#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/parsenum.h"
#include "py/repl.h"
#include "py/runtime.h"

#include "../snes/console.h"
#include "../snes/mailbox.h"
#include "../snes/oskb.h"

// Everything the REPL prints goes to BOTH channels: the mailbox ring (read
// by the Mesen harness / pytest) and the PPU text console (a real TV).
static void out_putc(char c)
{
  mb_putc(c);
  console_putc(c);
}

static void out_puts(const char *s)
{
  while (*s) {
    out_putc(*s++);
  }
}

// Input arrives from EITHER the mailbox stdin ring (scripted sessions) or
// the joypad on-screen keyboard. The wait loop is paced to one iteration
// per frame by console_flush()'s vblank wait.
static int in_getc(void)
{
  for (;;) {
    int c = mb_getc_nonblock();
    if (c >= 0) {
      return c;
    }
    c = oskb_poll();
    if (c >= 0) {
      return c;
    }
    console_flush();
  }
}

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
static void exec_input(vstr_t *line, mp_parse_input_kind_t kind)
{
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                vstr_str(line), line->len, 0);
    qstr source_name = lex->source_name;
    mp_parse_tree_t parse_tree = mp_parse(lex, kind);
    mp_obj_t module_fun = mp_compile(&parse_tree, source_name,
                                     kind == MP_PARSE_SINGLE_INPUT);
    mp_call_function_0(module_fun);
    nlr_pop();
  } else {
    mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
  }
}

static void exec_line(vstr_t *line)
{
  exec_input(line, MP_PARSE_SINGLE_INPUT);
}

// Read one logical input unit (following continuation prompts for compound
// statements) into line, echoing as we go. Returns 0 on Ctrl-D at the start
// of a line, 1 otherwise.
static int read_input(vstr_t *line)
{
  int at_line_start = 1;
  size_t line_start_len = 0;
  for (;;) {
    char c = (char)in_getc();
    if (c == 0x04 && at_line_start && line->len == 0) {
      return 0;
    }
    if (c == 0x01 && at_line_start && line->len == 0) {
      return 2; // raw mode (upstream-test runner): caller collects to ^D
    }
    if (c == '\r') {
      continue;
    }
    if (c == '\b' || c == 0x7f) {
      // joypad B: rub out within the current physical line only. The
      // mailbox log stays append-only (scripted sessions never backspace).
      if (line->len > line_start_len) {
        line->len--;
        console_putc('\b');
      }
      continue;
    }
    if (c == '\n') {
      out_putc('\n');
      if (mp_repl_continue_with_input(vstr_null_terminated_str(line))) {
        vstr_add_byte(line, '\n');
        out_puts("... ");
        at_line_start = 1;
        line_start_len = line->len;
        continue;
      }
      return 1;
    }
    vstr_add_byte(line, c);
    out_putc(c);
    at_line_start = 0;
  }
}

int main(void)
{
  int stack_dummy;
  mb_init();
  mp_cstack_init_with_top(&stack_dummy, STACK_SIZE);
  gc_init((void *)HEAP_START, (void *)HEAP_END);
  console_init();
  oskb_init();
  mp_init();
  #ifdef REPL_DEBUG
  {
    typedef union { float f; unsigned long u; } fu;
    static const float tf[4] = { 1.5f, 10.0f, 1e8f, 1e-8f };
    static volatile float vf = 10.0f;
    fu r;
    int i;
    mb_puts("romconst=");
    for (i = 0; i < 4; i++) {
      r.f = tf[i];
      mb_puthex32(r.u); // 3fc00000 41200000 4cbebc20 322bcc77
      mb_putc(' ');
    }
    r.f = vf;
    mb_puts("ramvar=");
    mb_puthex32(r.u);
    r.f = 0.15f * tf[1]; // 1.5: const-fold x ROM read
    mb_puts(" mul10=");
    mb_puthex32(r.u);
    mb_putc('\n');
    // parse "1.5" and "3.0" the way the compiler does
    r.f = mp_obj_get_float(mp_parse_num_float("1.5", 3, false, NULL));
    mb_puts("parse(1.5)=");
    mb_puthex32(r.u); // 3fc00000
    r.f = mp_obj_get_float(mp_parse_num_float("3.0", 3, false, NULL));
    mb_puts(" parse(3.0)=");
    mb_puthex32(r.u); // 40400000
    mb_putc('\n');
  }
  #endif
  #ifdef REPL_DEBUG
  {
    typedef union { mp_obj_t o; unsigned long u; } ou;
    ou r;
    mp_obj_t str_t = MP_OBJ_FROM_PTR(&mp_type_str);
    mp_obj_t *ha = m_new(mp_obj_t, 1);
    mp_obj_t sa[2];
    ha[0] = MP_OBJ_NEW_SMALL_INT(7);
    r.o = mp_call_function_n_kw(str_t, 1, 0, ha);
    mb_puts("heap-args:");
    mb_puthex32(r.u);
    sa[0] = MP_OBJ_NEW_SMALL_INT(7);
    r.o = mp_call_function_n_kw(str_t, 1, 0, sa);
    mb_puts(" stack-args:");
    mb_puthex32(r.u);
    r.o = mp_call_function_1(str_t, MP_OBJ_NEW_SMALL_INT(7));
    mb_puts(" fn1:");
    mb_puthex32(r.u);
    mb_putc(0x27);
    mp_obj_print_helper(&mp_plat_print, mp_call_function_n_kw(str_t, 1, 0, ha), PRINT_STR);
    mb_putc(0x27);
    mb_putc(10);
  }
  #endif
  out_puts("MicroPython on SNES; ^D exits\n"); // <= 32 cols: no line wrap
  vstr_t line;
  vstr_init(&line, 64);
  for (;;) {
    vstr_reset(&line);
    out_puts(">>> ");
    int r = read_input(&line);
    if (!r) {
      break;
    }
    if (r == 2) {
      // Raw mode, for tools/run_upstream_tests.py: collect source bytes
      // until ^D with no echo, run as a whole file, bracket the pure test
      // output in STX/EOT for the host to parse, then reset the
      // interpreter so tests don't see each other's globals.
      vstr_reset(&line);
      for (;;) {
        int c = in_getc();
        if (c == 0x04) {
          break;
        }
        vstr_add_byte(&line, (char)c);
      }
      mb_putc(0x02);
      exec_input(&line, MP_PARSE_FILE_INPUT);
      mb_putc(0x04);
      mp_deinit();
      mp_init();
      vstr_init(&line, 64); // old buffer belongs to the previous interpreter
      continue;
    }
    if (line.len == 0) {
      continue;
    }
    exec_line(&line);
  }
  out_puts("\nbye\n");
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

// No filesystem: the importer finds only frozen modules
mp_import_stat_t mp_import_stat(const char *path)
{
  (void)path;
  return MP_IMPORT_STAT_NO_EXIST;
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
    out_putc(*str++);
  }
  return n;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  mp_hal_stdout_tx_strn(str, len);
}
