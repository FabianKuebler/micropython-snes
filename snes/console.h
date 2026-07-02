// PPU text console: mode 0, BG1, 32x28 cells of 8x8 2bpp tiles, white on
// dark blue. A WRAM shadow tilemap is DMA'd to VRAM during vblank on
// console_flush(); nothing here uses interrupts (the REPL is synchronous,
// so we simply poll $4212). Rows 0..CON_SCROLL_ROWS-1 scroll as a terminal;
// the rows below are a static area owned by the on-screen keyboard (oskb.c),
// written via console_set_cell().
#ifndef SNES_CONSOLE_H
#define SNES_CONSOLE_H

#include <stdint.h>

#define CON_COLS 32
#define CON_ROWS 28
#define CON_SCROLL_ROWS 21 // terminal region; below: separator + keyboard

// tile index helpers: font tile = ascii - 0x20; +96 = highlighted variant
#define CON_TILE(c) ((uint16_t)((c) - 0x20))
#define CON_TILE_HL(c) ((uint16_t)((c) - 0x20 + 96))

void console_init(void); // full PPU bring-up, cleared screen, display on
void console_putc(char c); // terminal write: \n scrolls, \b rubs out
void console_puts(const char *s);
void console_set_cell(uint8_t x, uint8_t y, uint16_t tile); // static area
void console_flush(void); // wait for vblank, DMA shadow tilemap to VRAM

uint16_t pad_read(void); // JOY1 button bits (auto-joypad, polled)

#define PAD_B 0x8000u
#define PAD_Y 0x4000u
#define PAD_SELECT 0x2000u
#define PAD_START 0x1000u
#define PAD_UP 0x0800u
#define PAD_DOWN 0x0400u
#define PAD_LEFT 0x0200u
#define PAD_RIGHT 0x0100u
#define PAD_A 0x0080u
#define PAD_X 0x0040u
#define PAD_L 0x0020u
#define PAD_R 0x0010u

#endif
