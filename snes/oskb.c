#include "oskb.h"
#include "console.h"

#define KB_COLS 13
#define KB_ROWS 4
#define KB_TOP (CON_SCROLL_ROWS + 1) // row 21 is the separator line
#define KB_LEFT 3
#define REPEAT_DELAY 14 // frames before D-pad autorepeat kicks in
#define REPEAT_RATE 4

// exactly 13 chars per row; page 1 row 3 padding spaces just type a space
static const char *const kb_pages[2][KB_ROWS] = {
  {
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "0123456789._,",
    "()[]{}\"':=+-/",
  },
  {
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXYZ",
    "!@#$%^&*~`|\\;",
    "<>?          ",
  },
};

static uint8_t kb_page, kb_x, kb_y;
static uint16_t pad_prev;
static uint8_t rep_count;

static void draw_grid(void)
{
  uint8_t x, y;
  for (y = 0; y < KB_ROWS; y++) {
    const char *row = kb_pages[kb_page][y];
    for (x = 0; x < KB_COLS; x++) {
      char c = row[x];
      uint16_t tile = (x == kb_x && y == kb_y) ? CON_TILE_HL(c) : CON_TILE(c);
      console_set_cell(KB_LEFT + x * 2, KB_TOP + y, tile);
    }
  }
}

void oskb_init(void)
{
  uint8_t x;
  const char *hints = "A:type B:del X:spc Y:pg St:run";
  for (x = 0; x < CON_TEXT_COLS; x++) {
    console_set_cell(x, CON_SCROLL_ROWS, CON_TILE('-'));
    if (hints[x]) {
      console_set_cell(x, CON_TEXT_ROWS - 1, CON_TILE(hints[x]));
    }
  }
  kb_page = 0;
  kb_x = 0;
  kb_y = 0;
  pad_prev = 0;
  rep_count = 0;
  draw_grid();
}

// Called roughly once per frame while waiting for input (the caller's wait
// loop is paced by console_flush's vblank wait).
int oskb_poll(void)
{
  uint16_t pad = pad_read();
  uint16_t edge = pad & ~pad_prev;
  uint16_t dir = pad & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT);

  // D-pad: act on edge, then autorepeat while held
  uint16_t move = edge & dir;
  if (dir && dir == (pad_prev & (PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT))) {
    if (++rep_count >= REPEAT_DELAY) {
      rep_count -= REPEAT_RATE;
      move = dir;
    }
  } else {
    rep_count = 0;
  }
  pad_prev = pad;

  if (move) {
    if (move & PAD_LEFT) {
      kb_x = (kb_x == 0) ? KB_COLS - 1 : kb_x - 1;
    }
    if (move & PAD_RIGHT) {
      kb_x = (kb_x == KB_COLS - 1) ? 0 : kb_x + 1;
    }
    if (move & PAD_UP) {
      kb_y = (kb_y == 0) ? KB_ROWS - 1 : kb_y - 1;
    }
    if (move & PAD_DOWN) {
      kb_y = (kb_y == KB_ROWS - 1) ? 0 : kb_y + 1;
    }
    draw_grid();
    return -1;
  }
  if (edge & PAD_Y) {
    kb_page ^= 1;
    draw_grid();
    return -1;
  }
  if (edge & PAD_A) {
    return kb_pages[kb_page][kb_y][kb_x];
  }
  if (edge & PAD_B) {
    return '\b';
  }
  if (edge & PAD_X) {
    return ' ';
  }
  if (edge & PAD_START) {
    return '\n';
  }
  if (edge & PAD_SELECT) {
    return 0x04; // Ctrl-D
  }
  return -1;
}
