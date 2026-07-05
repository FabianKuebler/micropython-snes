// File manager UI (see fileman.h). Draws with console_set_cell over all 28
// rows (never console_putc, so nothing scrolls under us) and flushes once
// per frame — console_flush()'s vblank wait paces the input loop.
//
// Input comes from two planes, consumed strictly in order:
//  - joypad (humans + the test harness's JOYSEQ): private edge+autorepeat
//    state, cloned from oskb.c
//  - mailbox stdin (scripted tests): control bytes
//      0x05=A 0x06=B 0x0E=Y 0x10..0x13=up/down/left/right
//      0x04 = quit (in the list)
// Every state transition calls pad_wait_release() so a held button never
// leaks an edge into the next state.

#include "fileman.h"

#include <string.h>

#include "console.h"
#include "mailbox.h"
#include "oskb.h"
#include "sram_fs.h"

// null-separated frozen module names, empty-string terminated (frozenmod.c)
extern const char mp_frozen_names[];

#define REPEAT_DELAY 14
#define REPEAT_RATE 4

#define FM_MAX_ENTRIES (SFS_MAX_FILES + 12)

typedef struct {
  char name[20];
  uint8_t slot;   // SRAM slot, or 0xFF for frozen
  uint16_t size;  // SRAM only
} fm_entry_t;

static fm_entry_t fm_entries[FM_MAX_ENTRIES];
static uint8_t fm_n;
static uint8_t fm_sel;

static uint16_t fm_prev;
static uint16_t fm_rep;

void pad_wait_release(void)
{
  while (pad_read() != 0) {
    console_flush();
  }
  fm_prev = 0;
  fm_rep = 0;
}

// one frame: stdin byte first, else joypad edge/repeat; flushes the console
uint8_t ui_event(void)
{
  int c = mb_getc_nonblock();
  if (c >= 0) {
    switch (c) {
      case 0x05: return EV_A;
      case 0x06: return EV_B;
      case 0x0E: return EV_Y;
      case 0x10: return EV_UP;
      case 0x11: return EV_DOWN;
      case 0x04: return EV_QUIT;
      case 0x0F: return EV_SELECT;
      case 0x12: return EV_LEFT;
      case 0x13: return EV_RIGHT;
      default: return EV_NONE;
    }
  }
  {
    uint16_t now = pad_read();
    uint16_t pressed = (uint16_t)(now & (uint16_t)~fm_prev);
    uint16_t held = now;
    fm_prev = now;
    console_flush();
    if (pressed & PAD_A) return EV_A;
    if (pressed & PAD_B) return EV_B;
    if (pressed & PAD_Y) return EV_Y;
    if (pressed & PAD_SELECT) return EV_SELECT;
    if (pressed & PAD_START) return EV_START;
    if (pressed & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
      fm_rep = 0;
      if (pressed & PAD_UP) return EV_UP;
      if (pressed & PAD_DOWN) return EV_DOWN;
      if (pressed & PAD_LEFT) return EV_LEFT;
      return EV_RIGHT;
    }
    if (held & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT)) {
      fm_rep++;
      if (fm_rep >= REPEAT_DELAY && ((fm_rep - REPEAT_DELAY) % REPEAT_RATE) == 0) {
        if (held & PAD_UP) return EV_UP;
        if (held & PAD_DOWN) return EV_DOWN;
        if (held & PAD_LEFT) return EV_LEFT;
        return EV_RIGHT;
      }
    } else {
      fm_rep = 0;
    }
  }
  return EV_NONE;
}

void ui_text(uint8_t x, uint8_t y, const char *s, uint8_t hl)
{
  while (*s && x < CON_TEXT_COLS) {
    if (hl) {
      console_set_cell(x, y, CON_TILE_HL(*s));
    } else {
      console_set_cell(x, y, CON_TILE(*s));
    }
    x++;
    s++;
  }
}

void ui_fill_row(uint8_t y, char c, uint8_t hl)
{
  uint8_t x;
  for (x = 0; x < CON_TEXT_COLS; x++) {
    if (hl) {
      console_set_cell(x, y, CON_TILE_HL(c));
    } else {
      console_set_cell(x, y, CON_TILE(c));
    }
  }
}

void ui_put_u16(uint8_t x, uint8_t y, uint16_t v, uint8_t hl)
{
  char buf[6];
  uint8_t i = 0;
  if (v == 0) {
    buf[i++] = '0';
  }
  while (v > 0) {
    buf[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (i > 0) {
    i--;
    if (hl) {
      console_set_cell(x, y, CON_TILE_HL(buf[i]));
    } else {
      console_set_cell(x, y, CON_TILE(buf[i]));
    }
    x++;
  }
}

static void build_entries(void)
{
  uint8_t i;
  fm_n = 0;
  for (i = 0; i < SFS_MAX_FILES; i++) {
    if (!sfs_used(i)) {
      continue;
    }
    if (fm_n >= FM_MAX_ENTRIES) {
      break;
    }
    sfs_name(i, fm_entries[fm_n].name);
    fm_entries[fm_n].slot = i;
    fm_entries[fm_n].size = sfs_size(i);
    fm_n++;
  }
  {
    const char *p = mp_frozen_names;
    while (*p != '\0') {
      uint8_t skip = 0;
      const char *q = p;
      while (*q) {
        if (*q == '/') {
          skip = 1; // package internals: not directly runnable
        }
        q++;
      }
      if (!skip && fm_n < FM_MAX_ENTRIES) {
        uint8_t k = 0;
        while (p[k] != '\0' && k < 19) {
          fm_entries[fm_n].name[k] = p[k];
          k++;
        }
        fm_entries[fm_n].name[k] = '\0';
        fm_entries[fm_n].slot = 0xFF;
        fm_entries[fm_n].size = 0;
        fm_n++;
      }
      p = q + 1;
    }
  }
}

#define LIST_TOP 2
#define LIST_ROWS 17 // rows 2..18

static void draw_list(void)
{
  uint8_t r, i;
  uint8_t top = 0;
  if (fm_sel >= LIST_ROWS) {
    top = (uint8_t)(fm_sel - LIST_ROWS + 1);
  }
  for (r = 0; r < CON_ROWS; r++) {
    ui_fill_row(r, ' ', 0);
  }
  ui_text(0, 0, "MPY WORKSTATION", 0);
  ui_put_u16(19, 0, sfs_free_bytes(), 0);
  ui_text(25, 0, "b free", 0);
  for (r = 0; r < LIST_ROWS; r++) {
    i = (uint8_t)(top + r);
    if (i >= fm_n) {
      break;
    }
    {
      uint8_t hl = (uint8_t)(i == fm_sel);
      if (hl) {
        ui_fill_row((uint8_t)(LIST_TOP + r), ' ', 1);
      }
      if (fm_entries[i].slot == 0xFF) {
        ui_text(1, (uint8_t)(LIST_TOP + r), "[ROM]", hl);
        ui_text(7, (uint8_t)(LIST_TOP + r), fm_entries[i].name, hl);
      } else {
        ui_text(1, (uint8_t)(LIST_TOP + r), fm_entries[i].name, hl);
        ui_put_u16(22, (uint8_t)(LIST_TOP + r), fm_entries[i].size, hl);
      }
    }
  }
  ui_text(0, 20, "--------------------------------", 0);
  ui_text(0, CON_TEXT_ROWS - 1, "A:menu Y:new Sel:REPL St:quit", 0);
  // the flush-time block cursor doubles as the selection pointer
  // (otherwise it sits at (0,0) and covers the title's first letter)
  console_set_cursor(0, (uint8_t)(LIST_TOP + (fm_sel - top)));
}

// action menu over the selected entry; returns EV_A choice index or 0xFF
uint8_t ui_menu(const char *const *items, uint8_t n)
{
  uint8_t sel = 0;
  uint8_t i;
  for (;;) {
    for (i = 0; i < n; i++) {
      ui_fill_row((uint8_t)(6 + i), ' ', (uint8_t)(i == sel));
      ui_text(10, (uint8_t)(6 + i), items[i], (uint8_t)(i == sel));
    }
    {
      uint8_t ev = ui_event();
      if (ev == EV_UP && sel > 0) {
        sel--;
      } else if (ev == EV_DOWN && sel < (uint8_t)(n - 1)) {
        sel++;
      } else if (ev == EV_A) {
        pad_wait_release();
        return sel;
      } else if (ev == EV_B) {
        pad_wait_release();
        return 0xFF;
      }
    }
  }
}

void fileman_choose(fm_choice_t *out)
{
  build_entries();
  if (fm_sel >= fm_n) {
    fm_sel = 0;
  }
  mb_puts("fm: ");
  {
    char b[6];
    uint8_t i = 0, n = fm_n;
    if (n == 0) {
      mb_putc('0');
    }
    while (n > 0) {
      b[i++] = (char)('0' + (n % 10));
      n /= 10;
    }
    while (i > 0) {
      mb_putc(b[--i]);
    }
  }
  mb_puts(" entries\n");
  draw_list();
  pad_wait_release();

  for (;;) {
    uint8_t ev = ui_event();
    switch (ev) {
      case EV_UP:
        if (fm_sel > 0) {
          fm_sel--;
          draw_list();
        }
        break;
      case EV_DOWN:
        if ((uint8_t)(fm_sel + 1) < fm_n) {
          fm_sel++;
          draw_list();
        }
        break;
      case EV_SELECT:
        out->action = FM_REPL;
        mb_puts("fm: repl\n");
        pad_wait_release();
        return;
      case EV_START:
      case EV_QUIT:
        out->action = FM_QUIT;
        mb_puts("fm: quit\n");
        pad_wait_release();
        return;
      case EV_Y:
        out->action = FM_NEW;
        out->name[0] = '\0';
        mb_puts("fm: new\n");
        pad_wait_release();
        return; // os_main prompts for the name (editor flow)
      case EV_A:
        if (fm_n == 0) {
          break;
        }
        pad_wait_release();
        {
          fm_entry_t *e = &fm_entries[fm_sel];
          uint8_t pick;
          if (e->slot == 0xFF) {
            static const char *const rom_items[] = { "Run", "Cancel" };
            pick = ui_menu(rom_items, 2);
            if (pick == 0) {
              uint8_t k = 0;
              // strip trailing ".py" for the import name
              while (e->name[k] != '\0') {
                k++;
              }
              if (k > 3 && strcmp(&e->name[k - 3], ".py") == 0) {
                e->name[k - 3] = '\0';
              }
              out->action = FM_RUN_FROZEN;
              strcpy(out->name, e->name);
              mb_puts("fm: run ");
              mb_puts(e->name);
              mb_putc('\n');
              return;
            }
          } else {
            static const char *const file_items[] = { "Run", "Edit", "Delete", "Cancel" };
            pick = ui_menu(file_items, 4);
            if (pick == 0) {
              out->action = FM_RUN_FILE;
              out->slot = e->slot;
              strcpy(out->name, e->name);
              mb_puts("fm: run ");
              mb_puts(e->name);
              mb_putc('\n');
              return;
            }
            if (pick == 1) {
              out->action = FM_EDIT;
              out->slot = e->slot;
              strcpy(out->name, e->name);
              mb_puts("fm: edit ");
              mb_puts(e->name);
              mb_putc('\n');
              return;
            }
            if (pick == 2) {
              // confirm
              ui_fill_row(6, ' ', 1);
              ui_text(4, 6, "delete? A=yes B=no", 1);
              for (;;) {
                uint8_t e2 = ui_event();
                if (e2 == EV_A) {
                  sfs_delete(e->slot);
                  mb_puts("fm: del ");
                  mb_puts(e->name);
                  mb_putc('\n');
                  break;
                }
                if (e2 == EV_B) {
                  break;
                }
              }
              pad_wait_release();
              build_entries();
              if (fm_sel >= fm_n && fm_sel > 0) {
                fm_sel--;
              }
            }
          }
          draw_list();
        }
        break;
      default:
        break;
    }
  }
}
