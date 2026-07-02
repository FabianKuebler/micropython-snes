#include "console.h"

// MMIO, accessed as far pointers into bank $00 (same style as mailbox.h)
#define REG8(a) (*(volatile __far uint8_t *)(a))
#define INIDISP REG8(0x002100)
#define BGMODE REG8(0x002105)
#define BG1SC REG8(0x002107)
#define BG12NBA REG8(0x00210B)
#define BG1HOFS REG8(0x00210D)
#define BG1VOFS REG8(0x00210E)
#define VMAIN REG8(0x002115)
#define VMADDL REG8(0x002116)
#define VMADDH REG8(0x002117)
#define VMDATAL REG8(0x002118)
#define VMDATAH REG8(0x002119)
#define CGADD REG8(0x002121)
#define CGDATA REG8(0x002122)
#define TM REG8(0x00212C)
#define NMITIMEN REG8(0x004200)
#define MDMAEN REG8(0x00420B)
#define HVBJOY REG8(0x004212)
#define JOY1L REG8(0x004218)
#define JOY1H REG8(0x004219)
#define DMAP0 REG8(0x004300)
#define BBAD0 REG8(0x004301)
#define A1T0L REG8(0x004302)
#define A1T0H REG8(0x004303)
#define A1B0 REG8(0x004304)
#define DAS0L REG8(0x004305)
#define DAS0H REG8(0x004306)

// VRAM map (word addresses): font tiles at $0000, BG1 tilemap at $0400
#define VRAM_CHARBASE 0x0000
#define VRAM_TILEMAP 0x0400

// BGR555 palette: white text on C64-ish dark blue
#define COL_BG 0x5045u // r=5 g=2 b=20  -> deep blue
#define COL_FG 0x7FFFu // white

extern const unsigned char snes_font2bpp[]; // generated, 192 tiles

static uint16_t con_shadow[CON_ROWS * CON_COLS];
static uint8_t con_x, con_y;

// split a C far pointer into DMA source bytes without pointer->int casts
typedef union {
  const void *p;
  uint8_t b[4]; // little endian: b[0] lo, b[1] hi, b[2] bank
} farptr_t;

static void vram_dma(uint16_t vram_word_addr, const void *src, uint16_t bytes)
{
  farptr_t fp;
  fp.p = src;
  VMAIN = 0x80; // increment after high byte
  VMADDL = (uint8_t)vram_word_addr;
  VMADDH = (uint8_t)(vram_word_addr >> 8);
  DMAP0 = 0x01; // CPU->PPU, two registers write-once ($2118/$2119)
  BBAD0 = 0x18;
  A1T0L = fp.b[0];
  A1T0H = fp.b[1];
  A1B0 = fp.b[2];
  DAS0L = (uint8_t)bytes;
  DAS0H = (uint8_t)(bytes >> 8);
  MDMAEN = 0x01;
}

static void wait_vblank(void)
{
  while (HVBJOY & 0x80) { // if already in vblank, wait for it to end...
  }
  while (!(HVBJOY & 0x80)) { // ...then catch the start for a full window
  }
}

void console_flush(void)
{
  // overlay a block cursor at the write position for the DMA, then restore
  uint16_t idx = (uint16_t)con_y * CON_COLS + con_x;
  uint16_t saved = con_shadow[idx];
  con_shadow[idx] = CON_TILE_HL(' ');
  wait_vblank();
  vram_dma(VRAM_TILEMAP, con_shadow, sizeof(con_shadow));
  con_shadow[idx] = saved;
}

void console_set_cell(uint8_t x, uint8_t y, uint16_t tile)
{
  con_shadow[(uint16_t)y * CON_COLS + x] = tile;
}

static void clear_row(uint8_t y)
{
  uint8_t x;
  for (x = 0; x < CON_COLS; x++) {
    con_shadow[(uint16_t)y * CON_COLS + x] = CON_TILE(' ');
  }
}

static void scroll_up(void)
{
  uint16_t i;
  for (i = 0; i < (CON_SCROLL_ROWS - 1) * CON_COLS; i++) {
    con_shadow[i] = con_shadow[i + CON_COLS];
  }
  clear_row(CON_SCROLL_ROWS - 1);
}

void console_putc(char c)
{
  if (c == '\r') {
    return;
  }
  if (c == '\n') {
    con_x = 0;
    con_y++;
    if (con_y >= CON_SCROLL_ROWS) {
      scroll_up();
      con_y = CON_SCROLL_ROWS - 1;
    }
    console_flush(); // live output, one frame per line at worst
    return;
  }
  if (c == '\b') {
    if (con_x > 0) {
      con_x--;
      console_set_cell(con_x, con_y, CON_TILE(' '));
    }
    return;
  }
  if ((uint8_t)c < 0x20 || (uint8_t)c >= 0x80) {
    c = '?';
  }
  console_set_cell(con_x, con_y, CON_TILE(c));
  con_x++;
  if (con_x >= CON_COLS) {
    console_putc('\n');
  }
}

void console_puts(const char *s)
{
  while (*s) {
    console_putc(*s++);
  }
}

uint16_t pad_read(void)
{
  while (HVBJOY & 0x01) { // auto-joypad read in progress
  }
  return (uint16_t)JOY1L | ((uint16_t)JOY1H << 8);
}

void console_init(void)
{
  uint16_t i;
  INIDISP = 0x8F; // force blank
  BGMODE = 0x00; // mode 0: BG1 2bpp
  BG1SC = (VRAM_TILEMAP >> 10) << 2; // 32x32 tilemap
  BG12NBA = VRAM_CHARBASE >> 12;
  BG1HOFS = 0;
  BG1HOFS = 0;
  BG1VOFS = 0xFF; // -1: show tilemap row 0 on scanline 1
  BG1VOFS = 0x03;
  CGADD = 0;
  CGDATA = (uint8_t)COL_BG; // c0 backdrop / paper
  CGDATA = (uint8_t)(COL_BG >> 8);
  CGDATA = (uint8_t)COL_FG; // c1 text ink
  CGDATA = (uint8_t)(COL_FG >> 8);
  CGDATA = (uint8_t)COL_FG; // c2 highlight paper
  CGDATA = (uint8_t)(COL_FG >> 8);
  CGDATA = (uint8_t)COL_BG; // c3 highlight ink
  CGDATA = (uint8_t)(COL_BG >> 8);
  TM = 0x01; // BG1 on main screen
  vram_dma(VRAM_CHARBASE, snes_font2bpp, 192 * 16);
  con_x = 0;
  con_y = 0;
  for (i = 0; i < CON_ROWS * CON_COLS; i++) {
    con_shadow[i] = CON_TILE(' ');
  }
  vram_dma(VRAM_TILEMAP, con_shadow, sizeof(con_shadow));
  NMITIMEN = 0x01; // auto-joypad on, NMI off (we poll)
  INIDISP = 0x0F; // display on, full brightness
}
