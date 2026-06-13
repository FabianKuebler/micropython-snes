// HAL for the SNES port: no input, no clock; stdout is the test mailbox.
#include <stdint.h>

// Tell py/mphal.h we provide these inline (it otherwise emits a prototype,
// which vbcc rejects as a storage-class redeclaration of the inline below).
#define mp_hal_ticks_ms mp_hal_ticks_ms

static inline mp_uint_t mp_hal_ticks_ms(void)
{
  return 0;
}

static inline void mp_hal_set_interrupt_char(char c)
{
  (void)c;
}
