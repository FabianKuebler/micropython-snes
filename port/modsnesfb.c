// snesfb: MicroPython C module exposing a linear framebuffer on SNES video.
//
// The PPU has no bitmap mode, so init() builds one: mode 1, BG1 4bpp, and a
// tilemap whose first 24 rows reference 768 DISTINCT tiles — VRAM tile data
// then behaves as a 256x192, 16-color framebuffer. Python draws into a
// GS4_HMSB framebuf bytearray (2 px/byte, 24576 bytes) and show() converts
// packed nibbles to SNES bitplanes in a WRAM staging buffer, then DMAs it to
// VRAM in vblank-sized chunks (no interrupts; $4212 is polled, same approach
// as snes/console.c). palette(i, bgr555) programs CGRAM: nano-gui's 4-bit
// LUT driver model maps 1:1 onto the hardware palette.
//
// Linked into every ROM (module registration is global); costs nothing
// unless imported.

#include "py/obj.h"
#include "py/runtime.h"

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
#define MDMAEN REG8(0x00420B)
#define HVBJOY REG8(0x004212)
#define DMAP0 REG8(0x004300)
#define BBAD0 REG8(0x004301)
#define A1T0L REG8(0x004302)
#define A1T0H REG8(0x004303)
#define A1B0 REG8(0x004304)
#define DAS0L REG8(0x004305)
#define DAS0H REG8(0x004306)

#define FB_WIDTH 256
#define FB_HEIGHT 192
#define FB_TILES_X 32
#define FB_TILES_Y 24
#define FB_TILES (FB_TILES_X * FB_TILES_Y) // 768 unique tiles
#define FB_TILE_BYTES 32                   // 4bpp 8x8
#define FB_STAGE_BYTES (FB_TILES * FB_TILE_BYTES) // 24KB
#define FB_BUF_BYTES (FB_WIDTH / 2 * FB_HEIGHT)   // GS4: 24576

#define VRAM_CHARBASE 0x0000 // words; tiles 0..768
#define VRAM_TILEMAP 0x3800  // words
#define BLANK_TILE FB_TILES  // tile 768: all-zero, fills rows 24-31

// Planar staging buffer. Static (WRAM bank $7E, outside the GC heap bank),
// holds no pointers so the GC never needs to see it.
static uint8_t fb_stage[FB_STAGE_BYTES];

typedef union {
  const void *p;
  uint8_t b[4];
} farptr_t;

static void wait_vblank(void)
{
  while (HVBJOY & 0x80) {
  }
  while (!(HVBJOY & 0x80)) {
  }
}

static void vram_dma(uint16_t vram_word_addr, const void *src, uint16_t bytes)
{
  farptr_t fp;
  fp.p = src;
  VMAIN = 0x80;
  VMADDL = (uint8_t)vram_word_addr;
  VMADDH = (uint8_t)(vram_word_addr >> 8);
  DMAP0 = 0x01;
  BBAD0 = 0x18;
  A1T0L = fp.b[0];
  A1T0H = fp.b[1];
  A1B0 = fp.b[2];
  DAS0L = (uint8_t)bytes;
  DAS0H = (uint8_t)(bytes >> 8);
  MDMAEN = 0x01;
}

static mp_obj_t snesfb_init(void)
{
  uint16_t i;
  INIDISP = 0x8F; // force blank for the big VRAM setup
  BGMODE = 0x01;  // mode 1: BG1 4bpp
  BG1SC = (VRAM_TILEMAP >> 10) << 2;
  BG12NBA = VRAM_CHARBASE >> 12;
  BG1HOFS = 0;
  BG1HOFS = 0;
  BG1VOFS = 0xFF; // -1
  BG1VOFS = 0x03;
  TM = 0x01;

  // clear all tile data (768 fb tiles + blank tile)
  VMAIN = 0x80;
  VMADDL = 0;
  VMADDH = 0;
  for (i = 0; i < (FB_TILES + 1) * FB_TILE_BYTES / 2; i++) {
    VMDATAL = 0;
    VMDATAH = 0;
  }
  // tilemap: rows 0-23 -> unique tiles, rows 24-31 -> blank tile
  VMADDL = (uint8_t)VRAM_TILEMAP;
  VMADDH = (uint8_t)(VRAM_TILEMAP >> 8);
  for (i = 0; i < 32 * 32; i++) {
    uint16_t entry = (i < FB_TILES) ? i : BLANK_TILE;
    VMDATAL = (uint8_t)entry;
    VMDATAH = (uint8_t)(entry >> 8);
  }
  // palette: start black
  CGADD = 0;
  for (i = 0; i < 16; i++) {
    CGDATA = 0;
    CGDATA = 0;
  }
  INIDISP = 0x0F;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snesfb_init_obj, snesfb_init);

// palette(idx, bgr555): program CGRAM entry. Writes go to a shadow table;
// show() applies it at the start of its first vblank window. CGRAM writes
// during active display land at whatever address the PPU is rendering from
// (scrambled palette on hardware and accurate emulators), so they must be
// deferred to vblank.
static uint16_t cg_shadow[16];
static uint8_t cg_dirty;

static mp_obj_t snesfb_palette(mp_obj_t idx_in, mp_obj_t val_in)
{
  mp_int_t idx = mp_obj_get_int(idx_in);
  mp_int_t val = mp_obj_get_int(val_in);
  if (idx < 0 || idx > 15) {
    mp_raise_ValueError(MP_ERROR_TEXT("palette index 0-15"));
  }
  cg_shadow[idx] = (uint16_t)(val & 0x7FFF);
  cg_dirty = 1;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(snesfb_palette_obj, snesfb_palette);

static mp_obj_t snesfb_vsync(void)
{
  wait_vblank();
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snesfb_vsync_obj, snesfb_vsync);

// show(buf): GS4_HMSB (256x192, 24576 bytes) -> 4bpp planar -> VRAM.
static mp_obj_t snesfb_show(mp_obj_t buf_in)
{
  mp_buffer_info_t bufinfo;
  mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
  if (bufinfo.len != FB_BUF_BYTES) {
    mp_raise_ValueError(MP_ERROR_TEXT("buffer must be 256*192/2 bytes"));
  }
  const uint8_t *src = (const uint8_t *)bufinfo.buf;
  uint8_t ty, tx, row, k;

  // convert: source row-major nibbles, dest tile-major bitplanes
  for (ty = 0; ty < FB_TILES_Y; ty++) {
    for (tx = 0; tx < FB_TILES_X; tx++) {
      uint8_t *dst = &fb_stage[((uint16_t)ty * FB_TILES_X + tx) * FB_TILE_BYTES];
      for (row = 0; row < 8; row++) {
        const uint8_t *s = src + (((uint16_t)ty * 8 + row) * (FB_WIDTH / 2)) + (uint16_t)tx * 4;
        uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (k = 0; k < 4; k++) {
          uint8_t b = s[k];
          uint8_t hi = b >> 4, lo = b & 0x0F;
          uint8_t bit_hi = 7 - (k << 1), bit_lo = bit_hi - 1;
          p0 |= (uint8_t)((hi & 1) << bit_hi) | (uint8_t)((lo & 1) << bit_lo);
          p1 |= (uint8_t)(((hi >> 1) & 1) << bit_hi) | (uint8_t)(((lo >> 1) & 1) << bit_lo);
          p2 |= (uint8_t)(((hi >> 2) & 1) << bit_hi) | (uint8_t)(((lo >> 2) & 1) << bit_lo);
          p3 |= (uint8_t)(((hi >> 3) & 1) << bit_hi) | (uint8_t)(((lo >> 3) & 1) << bit_lo);
        }
        dst[row * 2] = p0;
        dst[row * 2 + 1] = p1;
        dst[16 + row * 2] = p2;
        dst[16 + row * 2 + 1] = p3;
      }
    }
  }

  // upload in vblank-sized chunks (4KB fits comfortably in one vblank);
  // a pending palette goes out at the start of the first window
  {
    uint16_t off;
    uint8_t i;
    for (off = 0; off < FB_STAGE_BYTES; off += 4096) {
      wait_vblank();
      if (cg_dirty) {
        CGADD = 0;
        for (i = 0; i < 16; i++) {
          CGDATA = (uint8_t)cg_shadow[i];
          CGDATA = (uint8_t)(cg_shadow[i] >> 8);
        }
        cg_dirty = 0;
      }
      vram_dma(VRAM_CHARBASE + off / 2, &fb_stage[off], 4096);
    }
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(snesfb_show_obj, snesfb_show);

static const mp_rom_map_elem_t snesfb_module_globals_table[] = {
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_snesfb) },
  { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&snesfb_init_obj) },
  { MP_ROM_QSTR(MP_QSTR_show), MP_ROM_PTR(&snesfb_show_obj) },
  { MP_ROM_QSTR(MP_QSTR_palette), MP_ROM_PTR(&snesfb_palette_obj) },
  { MP_ROM_QSTR(MP_QSTR_vsync), MP_ROM_PTR(&snesfb_vsync_obj) },
  { MP_ROM_QSTR(MP_QSTR_WIDTH), MP_ROM_INT(FB_WIDTH) },
  { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(FB_HEIGHT) },
};
static MP_DEFINE_CONST_DICT(snesfb_module_globals, snesfb_module_globals_table);

const mp_obj_module_t MICROPY_OBJ_BASE_ALIGNMENT mp_module_snesfb = {
  .base = { &mp_type_module },
  .globals = (mp_obj_dict_t *)&snesfb_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_snesfb, mp_module_snesfb);
