// Shared exec/REPL engine (see pyexec.h). Extracted verbatim from
// repl_main.c: the runtime hooks (gc_collect, nlr_jump_fail, stdout,
// lexer/import stubs) live here exactly once for the ROMs that link this
// TU (mpyrepl, mpyos); the frozen-module ROMs keep their own copies in
// port/main.c untouched.

#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"

#include "pyexec.h"

#include "../snes/console.h"
#include "../snes/mailbox.h"
#include "../snes/oskb.h"

// Everything printed goes to BOTH channels: the mailbox ring (read by the
// Mesen harness / pytest) and the PPU text console (a real TV).
void pyexec_putc(char c)
{
  mb_putc(c);
  console_putc(c);
}

void pyexec_puts(const char *s)
{
  while (*s) {
    pyexec_putc(*s++);
  }
}

// Input arrives from EITHER the mailbox stdin ring (scripted sessions) or
// the joypad on-screen keyboard. The wait loop is paced to one iteration
// per frame by console_flush()'s vblank wait.

// origin of the last byte pyexec_getc returned (1 = oskb/joypad). The REPL
// uses it to ignore joypad ^D (Select) in ROMs where exiting leads nowhere.
static uint8_t getc_from_pad;
// set per pyexec_repl() call: honor joypad ^D? (mpyos: yes, back to the
// file manager; mpyrepl: no, Select would just brick the console)
static uint8_t repl_pad_eof;

int pyexec_getc(void)
{
  for (;;) {
    int c = mb_getc_nonblock();
    if (c >= 0) {
      getc_from_pad = 0;
      return c;
    }
    c = oskb_poll();
    if (c >= 0) {
      getc_from_pad = 1;
      return c;
    }
    console_flush();
  }
}

// Compile and run one input unit. MP_PARSE_SINGLE_INPUT + is_repl makes
// bare expressions print their value (via __repl_print__), like CPython.
// NB: no locals are modified between nlr_push and the exception path
// (Calypsi drops volatile stores to stack locals in setjmp functions,
// DECISIONS.md bug 2).
void pyexec_str(const char *src, size_t len, mp_parse_input_kind_t kind)
{
  nlr_buf_t nlr;
  if (nlr_push(&nlr) == 0) {
    mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
                                                src, len, 0);
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

// Read one logical input unit (following continuation prompts for compound
// statements) into line, echoing as we go. Returns 0 on Ctrl-D at the start
// of a line, 1 on a complete unit, 2 on raw-mode escape (0x01).
static int read_input(vstr_t *line)
{
  int at_line_start = 1;
  size_t line_start_len = 0;
  for (;;) {
    char c = (char)pyexec_getc();
    if (c == 0x04 && at_line_start && line->len == 0) {
      if (repl_pad_eof || !getc_from_pad) {
        return 0;
      }
      continue; // joypad Select in a ROM with nowhere to exit to: ignore
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
      pyexec_putc('\n');
      if (mp_repl_continue_with_input(vstr_null_terminated_str(line))) {
        vstr_add_byte(line, '\n');
        pyexec_puts("... ");
        at_line_start = 1;
        line_start_len = line->len;
        continue;
      }
      return 1;
    }
    vstr_add_byte(line, c);
    pyexec_putc(c);
    at_line_start = 0;
  }
}

void pyexec_repl(int pad_eof)
{
  vstr_t line;
  repl_pad_eof = (uint8_t)(pad_eof != 0);
  if (repl_pad_eof) {
    pyexec_puts("MicroPython on SNES; Sel exits\n"); // <= 32 cols: no wrap
  } else {
    pyexec_puts("MicroPython on SNES\n");
  }
  vstr_init(&line, 64);
  for (;;) {
    vstr_reset(&line);
    pyexec_puts(">>> ");
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
        int c = pyexec_getc();
        if (c == 0x04) {
          break;
        }
        vstr_add_byte(&line, (char)c);
      }
      mb_putc(0x02);
      pyexec_str(vstr_str(&line), line.len, MP_PARSE_FILE_INPUT);
      mb_putc(0x04);
      mp_deinit();
      mp_init();
      vstr_init(&line, 64); // old buffer belongs to the previous interpreter
      continue;
    }
    if (line.len == 0) {
      continue;
    }
    pyexec_str(vstr_str(&line), line.len, MP_PARSE_SINGLE_INPUT);
  }
  pyexec_puts("\nbye\n");
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
    pyexec_putc(*str++);
  }
  return n;
}

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  mp_hal_stdout_tx_strn(str, len);
}
