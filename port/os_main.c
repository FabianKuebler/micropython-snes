// MPY WORKSTATION (build/mpyos.sfc): boots into a C file manager over the
// battery-SRAM file store; runs Python files and frozen demos in a fresh
// interpreter per run (raw-mode precedent: no state leaks, no C-held
// mp_obj_t survives a run); drops into the shared REPL on demand; edits
// files in the C full-screen editor.
//
// PPU ownership: the console owns it between runs. A program may hand it
// to snesfb/snesstage (which console_disable() the text screen); recover()
// takes it back after every run.

#include <string.h>

#include "py/cstack.h"
#include "py/gc.h"
#include "py/runtime.h"

#include "pyexec.h"

#include "../snes/console.h"
#include "../snes/editor.h"
#include "../snes/fileman.h"
#include "../snes/mailbox.h"
#include "../snes/oskb.h"
#include "../snes/sram_fs.h"

#define HEAP_START 0x7F0000UL
#define HEAP_END 0x7FE000UL
#define STACK_SIZE 0x1D00

void snesstage_hw_reset(void); // port/modsnesstage.c

// fresh heap + interpreter per program run
static void py_begin(void)
{
  gc_init((void *)HEAP_START, (void *)HEAP_END);
  mp_init();
}

static void py_end(void)
{
  mp_deinit();
}

// take the PPU back from whatever the program did with it
static void recover(void)
{
  snesstage_hw_reset();
  console_enable();
  console_init();
  pad_wait_release();
}

// level-triggered so a scripted ["b"]*N joypad tail catches it whenever
// the run ends; 0x06 is the stdin B-equivalent
static void wait_b(void)
{
  for (;;) {
    if (pad_read() & PAD_B) {
      return;
    }
    if (mb_getc_nonblock() == 0x06) {
      return;
    }
    console_flush(); // no-op while a demo owns the PPU; paces otherwise
  }
}

static void run_source(const char *buf, uint16_t len)
{
  py_begin();
  pyexec_str(buf, len, MP_PARSE_FILE_INPUT);
  pyexec_puts("\n== B: back ==\n");
  wait_b();
  mb_puts("os: back\n");
  py_end();
  recover();
}
// (both run paths share the tail shape; "os: back" marks wait_b's return
// for the tests)

// import the frozen module by name; the interpreter is fresh, so
// sys.modules is empty and the demo re-executes every run. NB: no locals
// modified between nlr_push and the exception arm (Calypsi setjmp rule).
static void run_frozen(const char *name)
{
  py_begin();
  {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
      mp_import_name(qstr_from_str(name), mp_const_none,
                     MP_OBJ_NEW_SMALL_INT(0));
      nlr_pop();
    } else {
      mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
  }
  pyexec_puts("\n== B: back ==\n");
  wait_b();
  mb_puts("os: back\n");
  py_end();
  recover();
}

// editor loop: Save+Run returns ED_RUN so saving and testing is one button
static void edit_loop(const char *name)
{
  for (;;) {
    uint8_t r = editor_session(name);
    if (r != ED_RUN) {
      break;
    }
    {
      uint16_t len;
      char *buf = editor_buffer(&len);
      run_source(buf, len);
    }
  }
}

int main(void)
{
  int stack_dummy;
  fm_choice_t ch;
  mb_init();
  mp_cstack_init_with_top(&stack_dummy, STACK_SIZE);
  console_init();
  sfs_mount();

  for (;;) {
    fileman_choose(&ch);
    switch (ch.action) {
      case FM_RUN_FILE: {
        uint16_t len;
        char *buf = editor_buffer(&len); // reuse the 8KB buffer
        len = sfs_read(ch.slot, buf, 8192);
        run_source(buf, len);
        break;
      }
      case FM_RUN_FROZEN:
        run_frozen(ch.name);
        break;
      case FM_EDIT:
        edit_loop(ch.name);
        break;
      case FM_NEW: {
        char name[24];
        if (editor_prompt_name(name, sizeof(name)) != 0) {
          edit_loop(name);
        }
        break;
      }
      case FM_REPL:
        py_begin();
        console_init(); // clear the manager screen for the terminal
        oskb_init();
        pyexec_repl(1); // Select exits back to the file manager
        py_end();
        recover();
        break;
      case FM_QUIT:
        mb_finish(MB_STATUS_PASS);
        break;
      default:
        break;
    }
  }
  return 0;
}
