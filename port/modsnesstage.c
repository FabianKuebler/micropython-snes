// _snesstage: hardware backend for the Stage game library
// (python-ugame/micropython-stage, MIT, by Radomir Dopieralski).
//
// Upstream Stage composites Banks (16 tiles of 16x16px, 16 colors), Grids
// (nibble tilemaps) and Sprites per-pixel in C and pushes RGB565 over SPI.
// On the SNES all of that is what the PPU does in silicon, so this module
// replaces the whole _stage renderer with register writes:
//   Bank   -> 4bpp character data, uploaded to VRAM once per bank
//   Grid   -> BG1 tilemap (mode 1, 16x16-pixel BG tiles), moved by scroll
//   Sprite -> an OAM entry (16x16 OBJ, hardware H/V flip)
// One char region at VRAM word 0 serves BG1 and OBJ both: 4bpp tiles have
// the same 32-byte format and the same 16x16 composition rule (chars n,
// n+1, n+16, n+17) on either side, and OBSEL/BG12NBA can point at the
// same base. A 16x16 tile t of bank slot s starts at char
// s*64 + (t%8)*2 + (t/8)*32, so a bank is 64 chars = 2048 bytes = exactly
// the Stage bank buffer size; 8 slots fill the 512-char OBJ name space.
//
// Nothing here touches the GC heap: all state is static WRAM shadows
// (tilemap, OAM, CGRAM) that flip() DMAs to the PPU in one vblank.
// Python-side buffers are read during the call only, never retained.
//
// Same no-interrupt discipline as modsnesfb.c: $4212 is polled, CGRAM/
// VRAM/OAM writes happen in vblank (or force blank during init).

#include "py/obj.h"
#include "py/runtime.h"

#include "../snes/console.h"

#define REG8(a) (*(volatile __far uint8_t *)(a))
#define INIDISP REG8(0x002100)
#define OBSEL REG8(0x002101)
#define OAMADDL REG8(0x002102)
#define OAMADDH REG8(0x002103)
#define BGMODE REG8(0x002105)
#define BG1SC REG8(0x002107)
#define BG12NBA REG8(0x00210B)
#define BG1HOFS REG8(0x00210D)
#define BG1VOFS REG8(0x00210E)
#define VMAIN REG8(0x002115)
#define VMADDL REG8(0x002116)
#define VMADDH REG8(0x002117)
#define CGADD REG8(0x002121)
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

#define VRAM_TILEMAP 0x3800 // words; 32x32 entries of 16x16px tiles
#define BLANK_NAME 832      // char in the cleared gap past the 8 bank slots
#define BANK_BYTES 2048
#define SCREEN_W 256
#define SCREEN_H 224

// PPU shadows (WRAM bank $7E, no pointers, invisible to the GC)
static uint16_t tm_shadow[32 * 32];
static uint8_t oam_shadow[544]; // 512 low table + 32 high table
static uint16_t cg_shadow[256];
static uint8_t bank_stage[BANK_BYTES];
static int16_t scroll_x, scroll_y;
static uint8_t st_ready, cg_dirty, tm_dirty;

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

// mode-0 DMA (single B-bus register) from a WRAM shadow
static void bbus_dma(uint8_t bbad, const void *src, uint16_t bytes)
{
  farptr_t fp;
  fp.p = src;
  DMAP0 = 0x00;
  BBAD0 = bbad;
  A1T0L = fp.b[0];
  A1T0H = fp.b[1];
  A1B0 = fp.b[2];
  DAS0L = (uint8_t)bytes;
  DAS0H = (uint8_t)(bytes >> 8);
  MDMAEN = 0x01;
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

// clear all 64KB of VRAM: fixed-source DMA reading a zero word
static const uint16_t zero_word = 0;
static void vram_clear_all(void)
{
  farptr_t fp;
  fp.p = &zero_word;
  VMAIN = 0x80;
  VMADDL = 0;
  VMADDH = 0;
  DMAP0 = 0x09; // mode 1 ($2118/19), fixed A-bus address
  BBAD0 = 0x18;
  A1T0L = fp.b[0];
  A1T0H = fp.b[1];
  A1B0 = fp.b[2];
  DAS0L = 0;
  DAS0H = 0; // 0 = 65536 bytes
  MDMAEN = 0x01;
}

static void apply_cgram(void)
{
  CGADD = 0;
  bbus_dma(0x22, cg_shadow, 512);
  cg_dirty = 0;
}

static void apply_oam(void)
{
  OAMADDL = 0;
  OAMADDH = 0;
  bbus_dma(0x04, oam_shadow, 544);
}

static void apply_scroll(void)
{
  uint16_t h = (uint16_t)(-scroll_x) & 0x3FF;
  uint16_t v = (uint16_t)(-scroll_y - 1) & 0x3FF;
  BG1HOFS = (uint8_t)h;
  BG1HOFS = (uint8_t)(h >> 8);
  BG1VOFS = (uint8_t)v;
  BG1VOFS = (uint8_t)(v >> 8);
}

// coordinates may be floats (games move sprites by fractional velocities);
// truncate toward zero like int()
static mp_int_t coord_int(mp_obj_t o)
{
  if (mp_obj_is_small_int(o)) {
    return MP_OBJ_SMALL_INT_VALUE(o);
  }
  return (mp_int_t)mp_obj_get_float(o);
}

// first char of 16x16 tile 'tile' in bank slot 'slot' (see header comment)
static uint16_t name_of(uint8_t slot, uint8_t tile)
{
  return (uint16_t)slot * 64 + (uint16_t)((tile & 7) << 1) +
         (uint16_t)((uint8_t)(tile >> 3) << 5);
}

static void ensure_init(void)
{
  uint16_t i;
  if (st_ready) {
    return;
  }
  st_ready = 1;
  console_disable(); // shared VRAM: the boot console's charset goes away
  INIDISP = 0x8F;    // force blank for the bulk setup
  BGMODE = 0x11;     // mode 1, BG1 chars 16x16
  BG1SC = (uint8_t)((VRAM_TILEMAP >> 10) << 2);
  BG12NBA = 0x00; // BG1 chars at word 0
  OBSEL = 0x00;   // OBJ chars at word 0, sizes 8x8/16x16 (all marked large)
  TM = 0x11;      // BG1 + OBJ
  vram_clear_all();
  for (i = 0; i < 32 * 32; i++) {
    tm_shadow[i] = BLANK_NAME;
  }
  for (i = 0; i < 512; i += 4) {
    oam_shadow[i] = 0;
    oam_shadow[i + 1] = 0xF0; // parked off-screen
    oam_shadow[i + 2] = 0;
    oam_shadow[i + 3] = 0;
  }
  for (i = 512; i < 544; i++) {
    oam_shadow[i] = 0xAA; // every sprite large (16x16), x bit 8 clear
  }
  for (i = 0; i < 256; i++) {
    cg_shadow[i] = 0;
  }
  scroll_x = 0;
  scroll_y = 0;
  apply_cgram();
  vram_dma(VRAM_TILEMAP, tm_shadow, sizeof(tm_shadow));
  apply_oam();
  apply_scroll();
  tm_dirty = 0;
  INIDISP = 0x0F;
}

static mp_obj_t snesstage_init(void)
{
  ensure_init();
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snesstage_init_obj, snesstage_init);

// bank(slot, graphic, palette): convert a Stage bank (2048 bytes, 16 tiles
// of 16x16 packed nibbles, even x = high nibble) into SNES 4bpp chars and
// DMA it to the slot's VRAM region; palette (16x RGB565 big-endian) is
// converted to BGR555 into the slot's BG and OBJ CGRAM shadows.
static mp_obj_t snesstage_bank(mp_obj_t slot_in, mp_obj_t gfx_in, mp_obj_t pal_in)
{
  mp_int_t slot = mp_obj_get_int(slot_in);
  mp_buffer_info_t gfx, pal;
  uint8_t t, sub, row, k, i;
  const uint8_t *g;
  const uint8_t *p;

  ensure_init();
  if (slot < 0 || slot > 7) {
    mp_raise_ValueError(MP_ERROR_TEXT("bank slot 0-7"));
  }
  mp_get_buffer_raise(gfx_in, &gfx, MP_BUFFER_READ);
  if (gfx.len != BANK_BYTES) {
    mp_raise_ValueError(MP_ERROR_TEXT("bank must be 2048 bytes"));
  }
  mp_get_buffer_raise(pal_in, &pal, MP_BUFFER_READ);
  if (pal.len != 32) {
    mp_raise_ValueError(MP_ERROR_TEXT("palette must be 32 bytes"));
  }
  g = (const uint8_t *)gfx.buf;
  p = (const uint8_t *)pal.buf;

  // tile t -> 4 chars (TL, TR, BL, BR), each 8x8 in 4 bitplanes
  for (t = 0; t < 16; t++) {
    uint16_t name = name_of(0, t); // slot offset applied at the DMA address
    for (sub = 0; sub < 4; sub++) {
      uint16_t n = name + (uint16_t)(sub & 1) + (uint16_t)((sub >> 1) ? 16 : 0);
      uint8_t *dst = &bank_stage[n * 32];
      uint8_t y0 = (uint8_t)((sub >> 1) << 3);
      uint8_t xb0 = (uint8_t)((sub & 1) << 2); // byte offset: 4 bytes = 8 px
      for (row = 0; row < 8; row++) {
        const uint8_t *s = g + (uint16_t)t * 128 + (uint16_t)(y0 + row) * 8 + xb0;
        uint8_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (k = 0; k < 4; k++) {
          uint8_t b = s[k];
          uint8_t hi = b >> 4, lo = b & 0x0F;
          uint8_t bit_hi = (uint8_t)(7 - (k << 1)), bit_lo = (uint8_t)(bit_hi - 1);
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

  // Stage palettes store RGB565 byte-swapped (high byte first)
  for (i = 0; i < 16; i++) {
    uint16_t c = (uint16_t)((uint16_t)p[(uint8_t)(i << 1)] << 8) |
                 (uint16_t)p[(uint8_t)((i << 1) + 1)];
    uint16_t r = (c >> 11) & 0x1F;
    uint16_t gr = (c >> 5) & 0x3F;
    uint16_t b = c & 0x1F;
    uint16_t v = (uint16_t)(b << 10) | (uint16_t)((gr >> 1) << 5) | r;
    cg_shadow[(uint8_t)(slot << 4) + i] = v;       // BG palette 'slot'
    cg_shadow[128 + (uint8_t)(slot << 4) + i] = v; // OBJ palette 'slot'
  }
  cg_dirty = 1;

  wait_vblank();
  vram_dma((uint16_t)slot * 1024, bank_stage, BANK_BYTES);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(snesstage_bank_obj, snesstage_bank);

// grid(map, stride, width, height, slot, pal): rebuild the tilemap shadow
// from a Stage nibble map (even x = high nibble); cells outside the grid
// show the blank char (backdrop color).
static mp_obj_t snesstage_grid(size_t n_args, const mp_obj_t *args)
{
  mp_buffer_info_t map;
  mp_int_t stride = mp_obj_get_int(args[1]);
  mp_int_t w = mp_obj_get_int(args[2]);
  mp_int_t h = mp_obj_get_int(args[3]);
  mp_int_t slot = mp_obj_get_int(args[4]);
  mp_int_t pal = mp_obj_get_int(args[5]);
  uint8_t x, y;
  const uint8_t *m;

  (void)n_args;
  ensure_init();
  if (slot < 0 || slot > 7 || pal < 0 || pal > 7) {
    mp_raise_ValueError(MP_ERROR_TEXT("slot/pal 0-7"));
  }
  if (w < 0 || w > 32 || h < 0 || h > 32 || stride < w) {
    mp_raise_ValueError(MP_ERROR_TEXT("grid up to 32x32"));
  }
  mp_get_buffer_raise(args[0], &map, MP_BUFFER_READ);
  if ((mp_int_t)map.len * 2 < stride * h) {
    mp_raise_ValueError(MP_ERROR_TEXT("map buffer too small"));
  }
  m = (const uint8_t *)map.buf;

  for (y = 0; y < 32; y++) {
    for (x = 0; x < 32; x++) {
      uint16_t entry = BLANK_NAME;
      if (x < (uint8_t)w && y < (uint8_t)h) {
        uint8_t b = m[((uint16_t)y * (uint16_t)stride + x) >> 1];
        uint8_t tile;
        if (x & 1) {
          tile = b & 0x0F;
        } else {
          tile = b >> 4;
        }
        entry = name_of((uint8_t)slot, tile) | (uint16_t)((uint16_t)pal << 10);
      }
      tm_shadow[(uint16_t)y * 32 + x] = entry;
    }
  }
  tm_dirty = 1;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(snesstage_grid_obj, 6, 6, snesstage_grid);

// scroll(x, y): position of the grid layer on screen (applied at flip)
static mp_obj_t snesstage_scroll(mp_obj_t x_in, mp_obj_t y_in)
{
  scroll_x = (int16_t)coord_int(x_in);
  scroll_y = (int16_t)coord_int(y_in);
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(snesstage_scroll_obj, snesstage_scroll);

// Stage rotation -> OAM attribute V/H flip bits. 90-degree rotations
// (odd values) have no hardware equivalent; the Python layer rejects them.
static const uint8_t rot_attr[8] = {0x00, 0x00, 0xC0, 0xC0, 0x40, 0x40, 0x80, 0x80};

// sprite(i, x, y, frame, rot, slot, pal): update OAM shadow entry i
static mp_obj_t snesstage_sprite(size_t n_args, const mp_obj_t *args)
{
  mp_int_t i = mp_obj_get_int(args[0]);
  mp_int_t x = coord_int(args[1]);
  mp_int_t y = coord_int(args[2]);
  mp_int_t frame = mp_obj_get_int(args[3]);
  mp_int_t rot = mp_obj_get_int(args[4]);
  mp_int_t slot = mp_obj_get_int(args[5]);
  mp_int_t pal = mp_obj_get_int(args[6]);
  uint16_t base, name, hb;
  uint8_t sh, mask, bits;

  (void)n_args;
  ensure_init();
  if (i < 0 || i > 127) {
    mp_raise_ValueError(MP_ERROR_TEXT("sprite index 0-127"));
  }
  if (frame < 0 || frame > 15 || slot < 0 || slot > 7 || pal < 0 || pal > 7) {
    mp_raise_ValueError(MP_ERROR_TEXT("frame 0-15, slot/pal 0-7"));
  }
  base = (uint16_t)i << 2;
  if (y <= -16 || y >= SCREEN_H || x <= -16 || x >= SCREEN_W) {
    oam_shadow[base + 1] = 0xF0; // fully off-screen: park it
    return mp_const_none;
  }
  name = name_of((uint8_t)slot, (uint8_t)frame);
  oam_shadow[base] = (uint8_t)x;
  oam_shadow[base + 1] = (uint8_t)y;
  oam_shadow[base + 2] = (uint8_t)name;
  oam_shadow[base + 3] = (uint8_t)(rot_attr[(uint8_t)rot & 7] | 0x30 |
                                   (uint8_t)((uint8_t)(pal & 7) << 1) |
                                   (uint8_t)((name >> 8) & 1));
  hb = 512 + (uint16_t)(i >> 2);
  sh = (uint8_t)((i & 3) << 1);
  mask = (uint8_t)((uint8_t)(3 << sh) ^ 0xFF);
  bits = (uint8_t)(2 | (uint8_t)((x >> 8) & 1)); // size=large, x bit 8
  oam_shadow[hb] = (uint8_t)((oam_shadow[hb] & mask) | (uint8_t)(bits << sh));
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(snesstage_sprite_obj, 7, 7, snesstage_sprite);

static mp_obj_t snesstage_hide(mp_obj_t i_in)
{
  mp_int_t i = mp_obj_get_int(i_in);
  if (i < 0 || i > 127) {
    mp_raise_ValueError(MP_ERROR_TEXT("sprite index 0-127"));
  }
  oam_shadow[((uint16_t)i << 2) + 1] = 0xF0;
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(snesstage_hide_obj, snesstage_hide);

// flip(): push all pending shadows to the PPU in one vblank.
// Worst case CGRAM 512 + tilemap 2048 + OAM 544 = ~3.1KB DMA, well inside
// the ~6KB a vblank allows.
static mp_obj_t snesstage_flip(void)
{
  ensure_init();
  wait_vblank();
  if (cg_dirty) {
    apply_cgram();
  }
  if (tm_dirty) {
    vram_dma(VRAM_TILEMAP, tm_shadow, sizeof(tm_shadow));
    tm_dirty = 0;
  }
  apply_scroll();
  apply_oam();
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(snesstage_flip_obj, snesstage_flip);

static mp_obj_t snesstage_vsync(mp_obj_t n_in)
{
  mp_int_t n = mp_obj_get_int(n_in);
  while (n > 0) {
    wait_vblank();
    n--;
  }
  return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(snesstage_vsync_obj, snesstage_vsync);

static const mp_rom_map_elem_t snesstage_module_globals_table[] = {
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__snesstage) },
  { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&snesstage_init_obj) },
  { MP_ROM_QSTR(MP_QSTR_bank), MP_ROM_PTR(&snesstage_bank_obj) },
  { MP_ROM_QSTR(MP_QSTR_grid), MP_ROM_PTR(&snesstage_grid_obj) },
  { MP_ROM_QSTR(MP_QSTR_scroll), MP_ROM_PTR(&snesstage_scroll_obj) },
  { MP_ROM_QSTR(MP_QSTR_sprite), MP_ROM_PTR(&snesstage_sprite_obj) },
  { MP_ROM_QSTR(MP_QSTR_hide), MP_ROM_PTR(&snesstage_hide_obj) },
  { MP_ROM_QSTR(MP_QSTR_flip), MP_ROM_PTR(&snesstage_flip_obj) },
  { MP_ROM_QSTR(MP_QSTR_vsync), MP_ROM_PTR(&snesstage_vsync_obj) },
  { MP_ROM_QSTR(MP_QSTR_WIDTH), MP_ROM_INT(SCREEN_W) },
  { MP_ROM_QSTR(MP_QSTR_HEIGHT), MP_ROM_INT(SCREEN_H) },
};
static MP_DEFINE_CONST_DICT(snesstage_module_globals, snesstage_module_globals_table);

const mp_obj_module_t MICROPY_OBJ_BASE_ALIGNMENT mp_module_snesstage = {
  .base = { &mp_type_module },
  .globals = (mp_obj_dict_t *)&snesstage_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__snesstage, mp_module_snesstage);
