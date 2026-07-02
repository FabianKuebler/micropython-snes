// Test mailbox polled by the Mesen Lua harness, at the top of WRAM bank $7F
// (out of bank 0, which belongs to the C stack — mp_init alone needs ~6KB
// and an overflow once chewed straight through the old $7E0100 mailbox).
// Layout:
//   $7FE000 u16 magic        0xCAFE = ROM alive
//   $7FE002 u16 status       0 running, 1 all passed, 0xDEAD panic
//   $7FE004 u16 write_index  stdout, SNES writes; monotonic, wraps at 65536
//   $7FE006 u16 in_windex    stdin, harness writes; monotonic
//   $7FE008 u16 in_rindex    stdin, SNES writes; monotonic (lets the
//                            harness compute free space)
//   $7FE010 u8[7664] ring    stdout ring buffer ($7FE010-$7FFDFF)
//   $7FFE00 u8[512] in_ring  stdin ring buffer ($7FFE00-$7FFFFF)
// The GC heap occupies the rest of bank $7F below ($7F0000-$7FDFFF).
// Index words are only ever written by one side, and the harness runs with
// the CPU paused (end-of-frame Lua callback), so no torn reads either way.
#ifndef SNES_MAILBOX_H
#define SNES_MAILBOX_H

#include <stdint.h>

#define MB_MAGIC_ALIVE 0xCAFEu
#define MB_STATUS_RUNNING 0u
#define MB_STATUS_PASS 1u
#define MB_STATUS_PANIC 0xDEADu

#define MB_RING_SIZE 7664u
#define MB_IN_RING_SIZE 512u

#define MB_MAGIC (*(volatile __far uint16_t *)0x7FE000)
#define MB_STATUS (*(volatile __far uint16_t *)0x7FE002)
#define MB_WINDEX (*(volatile __far uint16_t *)0x7FE004)
#define MB_IN_WINDEX (*(volatile __far uint16_t *)0x7FE006)
#define MB_IN_RINDEX (*(volatile __far uint16_t *)0x7FE008)
#define MB_RING ((volatile __far uint8_t *)0x7FE010)
#define MB_IN_RING ((volatile __far uint8_t *)0x7FFE00)

void mb_init(void);
void mb_putc(char c);
void mb_puts(const char *s); // writes the string, no newline added
void mb_puthex32(uint32_t v); // 8 hex digits, no prefix
int mb_getc_nonblock(void); // next stdin byte, or -1 if none pending
char mb_getc(void); // blocking stdin read (spins; harness feeds per frame)
void mb_finish(uint16_t status); // sets status and spins forever

#endif
