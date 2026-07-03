// Battery-SRAM file store. See sram_fs.h for the API and mapping notes.
//
// On-SRAM layout (logical addresses, all fields little-endian, written
// byte-wise — no struct overlays on far memory, no bitfields):
//   0x0000  u16 magic   0x4D46
//   0x0002  u16 version 1
//   0x0004  u16 data_end   logical addr of first free data byte
//   0x0006  u16 reserved
//   0x0008  16 x 20-byte dirent { char name[16] (NUL-padded, [0]==0 free),
//                                 u16 offset, u16 len }
//   0x0148..0x01FF reserved (zero)
//   0x0200..0x7FFF data area; live files packed contiguously ascending,
//                  free space is the single tail [data_end, 0x8000)

#include "sram_fs.h"

#include <string.h>

#define SFS_MAGIC 0x4D46u
#define SFS_VERSION 1u
#define ENT_ADDR(slot) (uint16_t)(0x0008u + (uint16_t)(slot) * 20u)

static uint8_t sfs_did_format;

// one 8KB window per 64K bank: $306000, $316000, $326000, $336000
static volatile __far uint8_t *sfs_ptr(uint16_t fa)
{
  uint32_t base = 0x306000UL + ((uint32_t)(fa >> 13) << 16);
  return (volatile __far uint8_t *)(base + (fa & 0x1FFFu));
}

static uint8_t peek8(uint16_t fa)
{
  return *sfs_ptr(fa);
}

static void poke8(uint16_t fa, uint8_t v)
{
  *sfs_ptr(fa) = v;
}

static uint16_t peek16(uint16_t fa)
{
  return (uint16_t)peek8(fa) | (uint16_t)((uint16_t)peek8(fa + 1) << 8);
}

static void poke16(uint16_t fa, uint16_t v)
{
  poke8(fa, (uint8_t)v);
  poke8(fa + 1, (uint8_t)(v >> 8));
}

static void copy_out(uint16_t fa, uint8_t *dst, uint16_t n)
{
  while (n > 0) {
    dst[0] = peek8(fa);
    dst++;
    fa++;
    n--;
  }
}

static void copy_in(uint16_t fa, const uint8_t *src, uint16_t n)
{
  while (n > 0) {
    poke8(fa, src[0]);
    src++;
    fa++;
    n--;
  }
}

// dst < src always (compaction shifts down), so forward copy is safe
static void move_down(uint16_t dst, uint16_t src, uint16_t n)
{
  while (n > 0) {
    poke8(dst, peek8(src));
    dst++;
    src++;
    n--;
  }
}

static void format(void)
{
  uint16_t a;
  for (a = 0; a < SFS_DATA_START; a++) {
    poke8(a, 0);
  }
  poke16(0x0000, SFS_MAGIC);
  poke16(0x0002, SFS_VERSION);
  poke16(0x0004, SFS_DATA_START);
  sfs_did_format = 1;
  // starter file so the first boot shows the whole workflow
  {
    static const char hello[] = "print(\"hello from SRAM\")\n";
    sfs_write("hello.py", hello, (uint16_t)(sizeof(hello) - 1));
  }
}

void sfs_mount(void)
{
  sfs_did_format = 0;
  if (peek16(0x0000) != SFS_MAGIC) {
    format();
    return;
  }
  if (peek16(0x0002) != SFS_VERSION) {
    format();
    return;
  }
  {
    uint16_t de = peek16(0x0004);
    if (de < SFS_DATA_START || de > SFS_DATA_LIMIT) {
      format();
    }
  }
}

uint8_t sfs_formatted(void)
{
  return sfs_did_format;
}

uint8_t sfs_used(uint8_t slot)
{
  if (slot >= SFS_MAX_FILES) {
    return 0;
  }
  if (peek8(ENT_ADDR(slot)) != 0) {
    return 1;
  }
  return 0;
}

uint8_t sfs_count(void)
{
  uint8_t i, n = 0;
  for (i = 0; i < SFS_MAX_FILES; i++) {
    if (sfs_used(i)) {
      n++;
    }
  }
  return n;
}

void sfs_name(uint8_t slot, char *out16)
{
  copy_out(ENT_ADDR(slot), (uint8_t *)out16, 16);
  out16[15] = '\0'; // dirents are NUL-padded; force-terminate anyway
}

uint16_t sfs_size(uint8_t slot)
{
  if (!sfs_used(slot)) {
    return 0;
  }
  return peek16(ENT_ADDR(slot) + 18);
}

int sfs_find(const char *name)
{
  uint8_t i;
  char buf[16];
  for (i = 0; i < SFS_MAX_FILES; i++) {
    if (!sfs_used(i)) {
      continue;
    }
    sfs_name(i, buf);
    if (strcmp(buf, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

uint16_t sfs_read(uint8_t slot, void *dst, uint16_t max)
{
  uint16_t off, len;
  if (!sfs_used(slot)) {
    return 0;
  }
  off = peek16(ENT_ADDR(slot) + 16);
  len = peek16(ENT_ADDR(slot) + 18);
  if (len > max) {
    len = max;
  }
  copy_out(off, (uint8_t *)dst, len);
  return len;
}

void sfs_delete(uint8_t slot)
{
  uint16_t gone_off, gone_len, data_end;
  uint8_t i;
  if (!sfs_used(slot)) {
    return;
  }
  gone_off = peek16(ENT_ADDR(slot) + 16);
  gone_len = peek16(ENT_ADDR(slot) + 18);
  data_end = peek16(0x0004);

  // compact: shift everything above the hole down by gone_len
  if (gone_len > 0 && gone_off + gone_len < data_end) {
    move_down(gone_off, gone_off + gone_len,
              (uint16_t)(data_end - (gone_off + gone_len)));
  }
  poke16(0x0004, (uint16_t)(data_end - gone_len));

  // clear the dirent, fix offsets of files that moved
  for (i = 0; i < 20; i++) {
    poke8(ENT_ADDR(slot) + i, 0);
  }
  for (i = 0; i < SFS_MAX_FILES; i++) {
    if (!sfs_used(i)) {
      continue;
    }
    {
      uint16_t o = peek16(ENT_ADDR(i) + 16);
      if (o > gone_off) {
        poke16(ENT_ADDR(i) + 16, (uint16_t)(o - gone_len));
      }
    }
  }
}

uint16_t sfs_free_bytes(void)
{
  return (uint16_t)(SFS_DATA_LIMIT - peek16(0x0004));
}

int sfs_write(const char *name, const void *src, uint16_t len)
{
  int existing = sfs_find(name);
  uint8_t slot = 0xFF;
  uint16_t data_end;
  uint8_t i;

  if (existing >= 0) {
    sfs_delete((uint8_t)existing); // rewrite = delete + append
  }
  if (len > sfs_free_bytes()) {
    return -1;
  }
  for (i = 0; i < SFS_MAX_FILES; i++) {
    if (!sfs_used(i)) {
      slot = i;
      break;
    }
  }
  if (slot == 0xFF) {
    return -2;
  }

  data_end = peek16(0x0004);
  copy_in(data_end, (const uint8_t *)src, len);
  {
    uint8_t n;
    for (n = 0; n < 16; n++) {
      char c = (n < 15) ? name[n] : '\0';
      poke8(ENT_ADDR(slot) + n, (uint8_t)c);
      if (c == '\0') {
        // NUL-pad the rest
        uint8_t k;
        for (k = (uint8_t)(n + 1); k < 16; k++) {
          poke8(ENT_ADDR(slot) + k, 0);
        }
        break;
      }
    }
  }
  poke16(ENT_ADDR(slot) + 16, data_end);
  poke16(ENT_ADDR(slot) + 18, len);
  poke16(0x0004, (uint16_t)(data_end + len));
  return 0;
}
