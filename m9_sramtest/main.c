// Battery-SRAM filesystem selftest (C only, no MicroPython): exercises
// snes/sram_fs.c through the mailbox so tests/test_sram.py can assert the
// transcript. Run twice against the same .srm to prove persistence: the
// first run formats and leaves files behind, the second mounts them.

#include <stdint.h>
#include <string.h>

#include "../snes/mailbox.h"
#include "../snes/sram_fs.h"

static uint8_t buf[8192];

static void put_u16(uint16_t v)
{
  char tmp[6];
  uint8_t i = 0;
  if (v == 0) {
    mb_putc('0');
    return;
  }
  while (v > 0) {
    tmp[i++] = (char)('0' + (v % 10));
    v /= 10;
  }
  while (i > 0) {
    mb_putc(tmp[--i]);
  }
}

static uint16_t checksum(const uint8_t *p, uint16_t n)
{
  uint16_t s = 0;
  while (n > 0) {
    s = (uint16_t)(s + *p);
    p++;
    n--;
  }
  return s;
}

static void fill_pattern(uint8_t *p, uint16_t n, uint8_t seed)
{
  uint16_t i;
  for (i = 0; i < n; i++) {
    p[i] = (uint8_t)(seed + (uint8_t)i);
  }
}

static void fail(const char *what)
{
  mb_puts("FAIL ");
  mb_puts(what);
  mb_putc('\n');
  mb_finish(MB_STATUS_PANIC);
}

int main(void)
{
  mb_init();
  sfs_mount();

  if (sfs_formatted()) {
    // ---- first run: fresh SRAM ----
    mb_puts("sram: formatted\n");
    mb_puts("sram: files ");
    put_u16(sfs_count()); // 1 (starter hello.py)
    mb_putc('\n');

    // file A: sized so file B's data spans the window-0/1 boundary
    // (data starts 0x0200; hello.py occupies ~25 bytes first)
    {
      uint16_t hello_len = sfs_size((uint8_t)sfs_find("hello.py"));
      uint16_t a_len = (uint16_t)(0x2000 - 0x0200 - hello_len - 50);
      fill_pattern(buf, a_len, 11);
      if (sfs_write("a.bin", buf, a_len) != 0) {
        fail("write a");
      }
      mb_puts("sram: a.bin len ");
      put_u16(a_len);
      mb_puts(" sum ");
      put_u16(checksum(buf, a_len));
      mb_putc('\n');
    }
    {
      uint16_t b_len = 200; // crosses logical 0x2000 (window 0 -> 1)
      fill_pattern(buf, b_len, 77);
      if (sfs_write("b.bin", buf, b_len) != 0) {
        fail("write b");
      }
      mb_puts("sram: b.bin len 200 sum ");
      put_u16(checksum(buf, b_len));
      mb_putc('\n');
    }
    // read-back checks
    {
      uint16_t n = sfs_read((uint8_t)sfs_find("b.bin"), buf, sizeof(buf));
      mb_puts("sram: b readback len ");
      put_u16(n);
      mb_puts(" sum ");
      put_u16(checksum(buf, n));
      mb_putc('\n');
    }
    // delete A -> compaction shifts b.bin down across the boundary
    sfs_delete((uint8_t)sfs_find("a.bin"));
    {
      uint16_t n = sfs_read((uint8_t)sfs_find("b.bin"), buf, sizeof(buf));
      mb_puts("sram: after delete b len ");
      put_u16(n);
      mb_puts(" sum ");
      put_u16(checksum(buf, n));
      mb_putc('\n');
    }
    mb_puts("sram: free ");
    put_u16(sfs_free_bytes());
    mb_putc('\n');

    // error paths: no-space and table-full
    if (sfs_write("huge.bin", buf, 0) != 0) {
      fail("zero write");
    }
    sfs_delete((uint8_t)sfs_find("huge.bin"));
    {
      // fill all remaining table slots with tiny files
      uint8_t i;
      char name[16];
      for (i = 0; i < SFS_MAX_FILES; i++) {
        if (sfs_count() >= SFS_MAX_FILES) {
          break;
        }
        name[0] = 'f';
        name[1] = (char)('a' + i);
        name[2] = '\0';
        if (sfs_write(name, buf, 4) != 0) {
          fail("fill write");
        }
      }
      if (sfs_count() != SFS_MAX_FILES) {
        fail("fill count");
      }
      if (sfs_write("onemore", buf, 4) != -2) {
        fail("table-full code");
      }
      // drop the fillers again, keep hello.py + b.bin for the 2nd run
      for (i = 0; i < SFS_MAX_FILES; i++) {
        char nm[16];
        if (!sfs_used(i)) {
          continue;
        }
        sfs_name(i, nm);
        if (nm[0] == 'f' && nm[2] == '\0') {
          sfs_delete(i);
        }
      }
    }
    if (sfs_write("nospace", buf,
                  (uint16_t)(sfs_free_bytes() + 1)) != -1) {
      fail("no-space code");
    }
    mb_puts("sram: files ");
    put_u16(sfs_count()); // 2: hello.py + b.bin
    mb_putc('\n');
  } else {
    // ---- second run: same .srm ----
    mb_puts("sram: mounted\n");
    mb_puts("sram: files ");
    put_u16(sfs_count());
    mb_putc('\n');
    {
      int b = sfs_find("b.bin");
      if (b < 0) {
        fail("persisted b.bin missing");
      }
      {
        uint16_t n = sfs_read((uint8_t)b, buf, sizeof(buf));
        mb_puts("sram: persisted b len ");
        put_u16(n);
        mb_puts(" sum ");
        put_u16(checksum(buf, n));
        mb_putc('\n');
      }
    }
  }

  mb_puts("sram: done\n");
  mb_finish(MB_STATUS_PASS);
  return 0;
}
