// Full-screen editor (see editor.h). One event per frame: mailbox stdin
// first (scripted tests: text bytes + 0x10-0x13 cursor moves + 0x04 menu),
// then the on-screen keyboard (oskb_poll: chars, '\b', '\n' via Start,
// 0x04 via Select -> menu). The MENU has a "Cursor mode" entry: in cursor
// focus the D-pad moves the text cursor instead of the keyboard selection
// (Select returns to typing, Start opens the menu).

#include "editor.h"

#include <string.h>

#include "console.h"
#include "fileman.h" // ui_* primitives + pad_wait_release
#include "mailbox.h"
#include "oskb.h"
#include "sram_fs.h"

#define ED_MAX 8192
#define VIEW_ROWS 21
#define STATUS_ROW 21

static char ed_buf[ED_MAX];
static uint16_t ed_len;
static uint16_t ed_cur;
static uint16_t ed_topline; // first visible line number
static uint8_t ed_dirty;
static char ed_name[24];
static uint8_t ed_flash; // frames left to show a status flash
static const char *ed_flash_msg;

char *editor_buffer(uint16_t *len_out)
{
  *len_out = ed_len;
  return ed_buf;
}

// line/column of a buffer offset (single forward scan)
static void locate(uint16_t off, uint16_t *line, uint8_t *col)
{
  uint16_t i, l = 0;
  uint8_t c = 0;
  for (i = 0; i < off; i++) {
    if (ed_buf[i] == '\n') {
      l++;
      c = 0;
    } else if (c < 255) {
      c++;
    }
  }
  *line = l;
  *col = c;
}

// buffer offset of the start of a line (scan; 0 if past the end)
static uint16_t line_start(uint16_t line)
{
  uint16_t i, l = 0;
  if (line == 0) {
    return 0;
  }
  for (i = 0; i < ed_len; i++) {
    if (ed_buf[i] == '\n') {
      l++;
      if (l == line) {
        return (uint16_t)(i + 1);
      }
    }
  }
  return ed_len;
}

static uint8_t line_len(uint16_t start)
{
  uint8_t n = 0;
  while ((uint16_t)(start + n) < ed_len && ed_buf[start + n] != '\n' && n < 255) {
    n++;
  }
  return n;
}

static void ed_draw(void)
{
  uint16_t cur_line;
  uint8_t cur_col;
  locate(ed_cur, &cur_line, &cur_col);

  // keep the cursor line in view
  if (cur_line < ed_topline) {
    ed_topline = cur_line;
  }
  if (cur_line >= (uint16_t)(ed_topline + VIEW_ROWS)) {
    ed_topline = (uint16_t)(cur_line - VIEW_ROWS + 1);
  }

  {
    uint16_t off = line_start(ed_topline);
    uint8_t r;
    for (r = 0; r < VIEW_ROWS; r++) {
      uint8_t x = 0;
      ui_fill_row(r, ' ', 0);
      while (off < ed_len && ed_buf[off] != '\n') {
        if (x < CON_TEXT_COLS) {
          console_set_cell(x, r, CON_TILE(ed_buf[off]));
        }
        x++;
        off++;
      }
      if (off < ed_len) {
        off++; // skip the newline
      } else {
        r++;
        while (r < VIEW_ROWS) {
          ui_fill_row(r, ' ', 0);
          r++;
        }
        break;
      }
    }
  }

  // status row: name, line:col, dirty flag (or a transient flash message)
  ui_fill_row(STATUS_ROW, ' ', 1);
  if (ed_flash > 0) {
    ed_flash--;
    ui_text(1, STATUS_ROW, ed_flash_msg, 1);
  } else {
    ui_text(1, STATUS_ROW, ed_name, 1);
    ui_text(17, STATUS_ROW, "L", 1);
    ui_put_u16(18, STATUS_ROW, (uint16_t)(cur_line + 1), 1);
    ui_text(23, STATUS_ROW, ":", 1);
    ui_put_u16(24, STATUS_ROW, (uint16_t)(cur_col + 1), 1);
    if (ed_dirty) {
      ui_text(30, STATUS_ROW, "*", 1);
    }
  }

  // the console_flush block cursor doubles as the text cursor
  {
    uint8_t x = cur_col;
    if (x > 31) {
      x = 31;
    }
    console_set_cursor(x, (uint8_t)(cur_line - ed_topline));
  }
}

static void flash(const char *msg)
{
  ed_flash_msg = msg;
  ed_flash = 90; // ~1.5s
}

static void ins(char c)
{
  if (ed_len >= ED_MAX) {
    flash("BUFFER FULL");
    return;
  }
  if (ed_cur < ed_len) {
    memmove(&ed_buf[ed_cur + 1], &ed_buf[ed_cur], (size_t)(ed_len - ed_cur));
  }
  ed_buf[ed_cur] = c;
  ed_cur++;
  ed_len++;
  ed_dirty = 1;
}

static void del_before(void)
{
  if (ed_cur == 0) {
    return;
  }
  if (ed_cur < ed_len) {
    memmove(&ed_buf[ed_cur - 1], &ed_buf[ed_cur], (size_t)(ed_len - ed_cur));
  }
  ed_cur--;
  ed_len--;
  ed_dirty = 1;
}

static void del_at(void)
{
  if (ed_cur >= ed_len) {
    return;
  }
  if ((uint16_t)(ed_cur + 1) < ed_len) {
    memmove(&ed_buf[ed_cur], &ed_buf[ed_cur + 1],
            (size_t)(ed_len - ed_cur - 1));
  }
  ed_len--;
  ed_dirty = 1;
}

static void move_vert(int8_t dir)
{
  uint16_t cur_line;
  uint8_t cur_col;
  locate(ed_cur, &cur_line, &cur_col);
  if (dir < 0 && cur_line == 0) {
    ed_cur = 0;
    return;
  }
  {
    uint16_t tgt = (dir < 0) ? (uint16_t)(cur_line - 1) : (uint16_t)(cur_line + 1);
    uint16_t start = line_start(tgt);
    uint8_t len;
    if (dir > 0 && start >= ed_len && cur_line + 1 > tgt) {
      // no next line
    }
    len = line_len(start);
    if (cur_col > len) {
      cur_col = len;
    }
    ed_cur = (uint16_t)(start + cur_col);
    if (ed_cur > ed_len) {
      ed_cur = ed_len;
    }
  }
}

static uint8_t ed_save(void)
{
  int r = sfs_write(ed_name, ed_buf, ed_len);
  if (r != 0) {
    flash("SRAM FULL");
    return 0;
  }
  ed_dirty = 0;
  mb_puts("ed: saved ");
  mb_puts(ed_name);
  mb_putc(' ');
  {
    char b[6];
    uint8_t i = 0;
    uint16_t v = ed_len;
    if (v == 0) {
      mb_putc('0');
    }
    while (v > 0) {
      b[i++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (i > 0) {
      mb_putc(b[--i]);
    }
  }
  mb_putc('\n');
  return 1;
}

// one merged input event for TEXT focus: stdin byte or oskb char, else -1
static int ed_getc_frame(void)
{
  int c = mb_getc_nonblock();
  if (c >= 0) {
    return c;
  }
  c = oskb_poll();
  return c; // -1 = nothing this frame
}

#define FOCUS_TEXT 0
#define FOCUS_CURSOR 1

uint8_t editor_session(const char *name)
{
  uint8_t focus = FOCUS_TEXT;

  strcpy(ed_name, name);
  ed_len = 0;
  {
    int slot = sfs_find(name);
    if (slot >= 0) {
      ed_len = sfs_read((uint8_t)slot, ed_buf, ED_MAX);
    }
  }
  ed_cur = ed_len;
  ed_topline = 0;
  ed_dirty = 0;
  ed_flash = 0;

  mb_puts("ed: open ");
  mb_puts(ed_name);
  mb_putc('\n');
  console_init(); // clean slate (wipes whatever screen came before)
  oskb_init();    // rows 21-27 (status row overwrites its separator)
  pad_wait_release();

  for (;;) {
    ed_draw();
    console_flush();

    if (focus == FOCUS_TEXT) {
      int c = ed_getc_frame();
      if (c < 0) {
        continue;
      }
      if (c == 0x04) {
        goto menu;
      }
      if (c == 0x10 || c == 0x11 || c == 0x12 || c == 0x13) {
        // stdin cursor moves (tests)
        if (c == 0x10) {
          move_vert(-1);
        } else if (c == 0x11) {
          move_vert(1);
        } else if (c == 0x12 && ed_cur > 0) {
          ed_cur--;
        } else if (c == 0x13 && ed_cur < ed_len) {
          ed_cur++;
        }
        continue;
      }
      if (c == '\b' || c == 0x7f) {
        del_before();
        continue;
      }
      if (c == '\r') {
        continue;
      }
      if (c == '\n' || (c >= 0x20 && c < 0x7f)) {
        ins((char)c);
      }
      continue;
    }

    // FOCUS_CURSOR: raw pad via ui_event
    {
      uint8_t ev = ui_event();
      switch (ev) {
        case EV_UP: move_vert(-1); break;
        case EV_DOWN: move_vert(1); break;
        case EV_LEFT:
          if (ed_cur > 0) {
            ed_cur--;
          }
          break;
        case EV_RIGHT:
          if (ed_cur < ed_len) {
            ed_cur++;
          }
          break;
        case EV_A: del_at(); break;
        case EV_B: del_before(); break;
        case EV_SELECT:
          focus = FOCUS_TEXT;
          pad_wait_release();
          break;
        case EV_START:
        case EV_QUIT:
          goto menu;
        default:
          break;
      }
      continue;
    }

  menu:
    pad_wait_release();
    {
      static const char *const items[] = {
        "Resume", "Cursor mode", "Save", "Save+Run", "Save+Exit", "Discard",
      };
      uint8_t pick = ui_menu(items, 6);
      switch (pick) {
        case 1:
          focus = (uint8_t)(focus == FOCUS_TEXT ? FOCUS_CURSOR : FOCUS_TEXT);
          break;
        case 2:
          ed_save();
          break;
        case 3:
          if (ed_save()) {
            return ED_RUN;
          }
          break;
        case 4:
          if (ed_save()) {
            return ED_EXIT;
          }
          break;
        case 5:
          if (ed_dirty) {
            ui_fill_row(6, ' ', 1);
            ui_text(4, 6, "discard changes? A=yes B=no", 1);
            for (;;) {
              uint8_t e2 = ui_event();
              if (e2 == EV_A) {
                pad_wait_release();
                return ED_EXIT;
              }
              if (e2 == EV_B) {
                break;
              }
            }
            pad_wait_release();
          } else {
            return ED_EXIT;
          }
          break;
        default:
          break; // Resume / cancel
      }
      // full redraw after the overlay
      console_init();
      oskb_init();
      pad_wait_release();
    }
  }
}

uint8_t editor_prompt_name(char *out, uint8_t maxlen)
{
  uint8_t n = 0;
  console_init();
  oskb_init();
  ui_text(2, 18, "new file name:", 0);
  pad_wait_release();
  for (;;) {
    // render the name + cursor
    ui_fill_row(19, ' ', 0);
    ui_text(2, 19, out, 0);
    console_set_cursor((uint8_t)(2 + n), 19);
    console_flush();
    {
      int c = mb_getc_nonblock();
      if (c < 0) {
        c = oskb_poll();
      }
      if (c < 0) {
        continue;
      }
      if (c == 0x04) {
        return 0; // cancelled
      }
      if (c == '\b' || c == 0x7f) {
        if (n > 0) {
          n--;
          out[n] = '\0';
        }
        continue;
      }
      if (c == '\n') {
        if (n == 0) {
          return 0;
        }
        // append .py if missing (and if it fits the sfs name limit)
        if (n < 3 || strcmp(&out[n - 3], ".py") != 0) {
          if ((uint8_t)(n + 3) < maxlen && n + 3 <= SFS_NAME_MAX) {
            strcpy(&out[n], ".py");
            n = (uint8_t)(n + 3);
          }
        }
        mb_puts("fm: name ");
        mb_puts(out);
        mb_putc('\n');
        pad_wait_release();
        return 1;
      }
      if (c >= 0x20 && c < 0x7f && n < (uint8_t)(maxlen - 1) && n < SFS_NAME_MAX) {
        out[n++] = (char)c;
        out[n] = '\0';
      }
    }
  }
}
