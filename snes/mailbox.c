#include "mailbox.h"

void mb_init(void)
{
  MB_WINDEX = 0;
  MB_STATUS = MB_STATUS_RUNNING;
  MB_MAGIC = MB_MAGIC_ALIVE;
}

void mb_putc(char c)
{
  uint16_t wi = MB_WINDEX;
  MB_RING[wi % MB_RING_SIZE] = (uint8_t)c;
  MB_WINDEX = wi + 1;
}

void mb_puts(const char *s)
{
  while (*s) {
    mb_putc(*s++);
  }
}

void mb_finish(uint16_t status)
{
  MB_STATUS = status;
  for (;;) {
  }
}
