// File manager UI for the workstation ROM: full-screen list over the
// battery-SRAM files plus the frozen [ROM] modules. Pure UI — it returns a
// choice to os_main and never touches the interpreter.
#ifndef SNES_FILEMAN_H
#define SNES_FILEMAN_H

#include <stdint.h>

enum {
  FM_RUN_FILE = 1,  // slot = SRAM table slot
  FM_RUN_FROZEN = 2, // name = frozen module name (without .py)
  FM_EDIT = 3,      // slot = SRAM table slot (0xFF = new file, see name)
  FM_NEW = 4,       // name = new file name (with .py)
  FM_REPL = 5,
  FM_QUIT = 6,
};

typedef struct {
  uint8_t action;
  uint8_t slot;
  char name[24];
} fm_choice_t;

// draw the manager and loop until the user picks something
void fileman_choose(fm_choice_t *out);

// wait until no joypad button is held (call on every UI state transition:
// oskb_init/fm redraws reset edge state, so a held button would otherwise
// leak a fresh edge into the next state). Lives in fileman.c.
void pad_wait_release(void);

// ---- shared UI primitives (fileman.c; also used by editor.c) ---------------

// input events: one per frame, stdin control bytes take priority over the
// joypad (stdin: 0x05=A 0x06=B 0x0E=Y 0x0F=Select 0x10..0x13=U/D/L/R,
// 0x04=quit/menu depending on state)
#define EV_NONE 0
#define EV_UP 1
#define EV_DOWN 2
#define EV_LEFT 3
#define EV_RIGHT 4
#define EV_A 5
#define EV_B 6
#define EV_Y 7
#define EV_SELECT 8
#define EV_START 9
#define EV_QUIT 10

uint8_t ui_event(void); // one frame: stdin byte, else pad edge/repeat
void ui_text(uint8_t x, uint8_t y, const char *s, uint8_t hl);
void ui_fill_row(uint8_t y, char c, uint8_t hl);
void ui_put_u16(uint8_t x, uint8_t y, uint16_t v, uint8_t hl);
// modal list overlay at rows 6..; returns index or 0xFF on B/cancel
uint8_t ui_menu(const char *const *items, uint8_t n);

#endif
