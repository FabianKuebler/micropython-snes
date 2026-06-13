/* vbcc spike: M0-equivalent hello to the test mailbox at $7FE000. */
#define MB_MAGIC  (*(volatile __huge unsigned short *)0x7FE000)
#define MB_STATUS (*(volatile __huge unsigned short *)0x7FE002)
#define MB_WINDEX (*(volatile __huge unsigned short *)0x7FE004)
#define MB_RING   ((volatile __huge unsigned char *)0x7FE010)

static void mb_putc(char c) {
    unsigned short wi = MB_WINDEX;
    MB_RING[wi % 8176u] = (unsigned char)c;
    MB_WINDEX = wi + 1;
}
static void mb_puts(const char *s) { while (*s) mb_putc(*s++); }

int main(void) {
    MB_WINDEX = 0;
    MB_STATUS = 0;
    MB_MAGIC = 0xCAFE;
    mb_puts("Hello from vbcc on SNES\n");
    mb_puts("vbcc M0 done\n");
    MB_STATUS = 1;
    for (;;) {}
    return 0;
}
