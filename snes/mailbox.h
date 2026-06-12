// Test mailbox at fixed WRAM addresses, polled by the Mesen Lua harness.
// Layout (see DECISIONS.md): $7E0100 magic, $7E0102 status, $7E0104
// write_index (monotonic u16), $7E0110 ring buffer of 3824 bytes.
#ifndef SNES_MAILBOX_H
#define SNES_MAILBOX_H

#include <stdint.h>

#define MB_MAGIC_ALIVE 0xCAFEu
#define MB_STATUS_RUNNING 0u
#define MB_STATUS_PASS 1u
#define MB_STATUS_PANIC 0xDEADu

#define MB_RING_SIZE 3824u

#define MB_MAGIC (*(volatile __far uint16_t *)0x7e0100)
#define MB_STATUS (*(volatile __far uint16_t *)0x7e0102)
#define MB_WINDEX (*(volatile __far uint16_t *)0x7e0104)
#define MB_RING ((volatile __far uint8_t *)0x7e0110)

void mb_init(void);
void mb_putc(char c);
void mb_puts(const char *s); // writes the string, no newline added
void mb_finish(uint16_t status); // sets status and spins forever

#endif
