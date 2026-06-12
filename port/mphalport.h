// HAL for the SNES port: no input, no clock; stdout is the test mailbox.
#include <stdint.h>

static inline mp_uint_t mp_hal_ticks_ms(void)
{
  return 0;
}

static inline void mp_hal_set_interrupt_char(char c)
{
  (void)c;
}
