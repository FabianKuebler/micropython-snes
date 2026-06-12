// M1: trust-but-verify the Calypsi compiler on the SNES, in the emulator.
// Exercises what the MicroPython VM leans on: 32/64-bit arithmetic, pointer
// arithmetic across banks, struct copy/by-value, switch, function pointers,
// setjmp/longjmp (NLR), varargs, recursion, and the C library.
//
// All operands come from volatile globals so the compiler must generate the
// operations at runtime instead of constant-folding them on the host.
// Output protocol: one "ok <name>" / "FAIL <name>" line per check, in fixed
// order (tests/test_selftest.py asserts the exact transcript).

#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "../snes/mailbox.h"

static unsigned int fails;

static void report(const char *name, int ok)
{
  mb_puts(ok ? "ok " : "FAIL ");
  mb_puts(name);
  mb_putc('\n');
  if (!ok) {
    fails++;
  }
}

static void report32(const char *name, uint32_t got, uint32_t want)
{
  report(name, got == want);
  if (got != want) {
    mb_puts("  got ");
    mb_puthex32(got);
    mb_puts(" want ");
    mb_puthex32(want);
    mb_putc('\n');
  }
}

static void report64(const char *name, uint64_t got, uint64_t want)
{
  report(name, got == want);
  if (got != want) {
    mb_puts("  got ");
    mb_puthex32((uint32_t)(got >> 32));
    mb_puthex32((uint32_t)got);
    mb_puts(" want ");
    mb_puthex32((uint32_t)(want >> 32));
    mb_puthex32((uint32_t)want);
    mb_putc('\n');
  }
}

// ---- volatile operand pool ------------------------------------------------

static volatile uint32_t vA = 0x12345678UL;
static volatile uint32_t vB = 0x00010001UL;
static volatile uint32_t vB2 = 0x0000FFFFUL;
static volatile int32_t vN = -100000L;
static volatile int32_t vD = 7L;
static volatile int32_t vMin = INT32_MIN;
static volatile uint16_t vs4 = 4, vs8 = 8, vs36 = 36;
static volatile uint64_t vX = 0x123456789AULL;
static volatile uint64_t vF = 0xFFFFFFFFULL;
static volatile uint64_t vP = 0x100000001ULL;
static volatile uint64_t vT = 1ULL << 40;
static volatile int64_t vM = -1000000000000LL;
static volatile int16_t vi2 = 2, vi5 = 5;
static volatile int32_t vl1 = 100000L, vl2 = -5L, vl3 = 123456L;
static volatile uint16_t vop0 = 0, vop1 = 1, vop2 = 2;

// ---- 32-bit arithmetic ----------------------------------------------------

static void test_u32(void)
{
  uint32_t a = vA, b = vB;
  report32("u32.add", a + b, 0x12355679UL);
  report32("u32.sub", a - b, 0x12335677UL);
  report32("u32.mul", a * b, 0x68AC5678UL);
  report32("u32.div", a / b, 0x1234UL);
  report32("u32.mod", a % b, 0x4444UL);
  report32("u32.shl", a << vs4, 0x23456780UL);
  report32("u32.shr", a >> vs8, 0x00123456UL);
  // unsigned compare with the sign bit set: a classic codegen trap
  report("u32.cmp", (a | 0x80000000UL) > 0x7FFFFFFFUL && a < 0xFFFFFFFFUL &&
                    !(a > a) && a >= vA);
}

static void test_i32(void)
{
  int32_t n = vN, d = vD, m = vMin;
  report32("i32.div", (uint32_t)(n / d), (uint32_t)-14285L);
  report32("i32.mod", (uint32_t)(n % d), (uint32_t)-5L);
  report32("i32.sar", (uint32_t)(n >> vs4), (uint32_t)-6250L);
  report32("i32.neg", (uint32_t)-n, 100000UL);
  report("i32.cmp", m < 0L && (int32_t)(m + 1) == -2147483647L && n < d);
}

// ---- 64-bit arithmetic ----------------------------------------------------

static void test_u64(void)
{
  uint64_t x = vX, f = vF, p = vP, t = vT;
  report64("u64.add", x + f, 0x1334567899ULL);
  report64("u64.mul", p * p, 0x0000000200000001ULL);
  report64("u64.shl", x << (vs36 - 29), 0x91A2B3C4D00ULL); // shift by 7
  report64("u64.shr", x >> (vs4 + vs8), 0x1234567ULL);    // shift by 12
  report("u64.div", t / 3 == 0x5555555555ULL && t % 3 == 1ULL);
}

static void test_i64(void)
{
  int64_t m = vM;
  report64("i64.div", (uint64_t)(m / 7), (uint64_t)-142857142857LL);
  report64("i64.mod", (uint64_t)(m % 7), (uint64_t)-1LL);
}

// ---- far pointer arithmetic, both WRAM banks --------------------------------
// Calypsi far pointers/objects must stay within one 64K bank (manual 5.3.3);
// arithmetic does not carry into the bank byte. Verified the hard way — see
// DECISIONS.md. The port keeps every object (GC heap!) inside a single bank.

static void test_ptr(void)
{
  // high in bank $7E, untouched WRAM (data lives at $7E2000+, well below)
  volatile uint8_t *p = (volatile uint8_t *)0x7EFF00UL;
  // bank $7F, far above anything allocated
  volatile uint8_t *r = (volatile uint8_t *)0x7F8000UL;
  int16_t i;
  for (i = 0; i < 16; i++) {
    p[i] = (uint8_t)(0x37 + i * 5);
    r[i * 2] = (uint8_t)(i + 1);
  }
  report("ptr.farhi", p[0] == 0x37 && *(p + 10) == 0x69 && p[15] == 0x82);
  report("ptr.bank7f", r[0] == 1 && *(r + 20) == 11 && r[30] == 16);
  report("ptr.diff", ((p + 10) - p) == 10 && (r + 30) - (r + 8) == 22);
}

// ---- struct copy and pass/return by value ----------------------------------

typedef struct {
  char tag[6];
  uint16_t u;
  uint32_t x, y;
  int16_t arr[5];
} Blob;

static Blob bump(Blob b)
{
  b.x += 0x1111UL;
  b.arr[0]++;
  return b;
}

static void test_struct(void)
{
  Blob b1, b2, b3;
  strcpy(b1.tag, "blob1");
  b1.u = (uint16_t)vs8;
  b1.x = vA;
  b1.y = vB2;
  b1.arr[0] = 10;
  b1.arr[2] = (int16_t)vi5;
  b1.arr[4] = -42;
  b2 = b1;
  b1.x ^= 0xFFFFUL;
  b1.arr[2] = 999;
  report("struct.copy", b2.x == 0x12345678UL && b2.arr[2] == 5 &&
                        b2.arr[4] == -42 && strcmp(b2.tag, "blob1") == 0);
  b3 = bump(b2);
  report("struct.byval", b3.x == 0x12346789UL && b3.arr[0] == 11 &&
                         b2.x == 0x12345678UL && b2.arr[0] == 10);
}

// ---- switch dispatch --------------------------------------------------------

static int16_t classify_dense(int16_t v)
{
  switch (v) {
  case 0: return 1;
  case 1: return 4;
  case 2: return 7;
  case 3: return 10;
  case 4: return 13;
  case 5: return 16;
  case 6: return 19;
  case 7: return 22;
  default: return -1;
  }
}

static int16_t classify_sparse(int16_t v)
{
  switch (v) {
  case 1: return 11;
  case 7: return 22;
  case 100: return 33;
  case 1000: return 44;
  case 30000: return 55;
  default: return 66;
  }
}

static int16_t classify_long(int32_t v)
{
  switch (v) {
  case 100000L: return 11;
  case 200000L: return 22;
  case -5L: return 33;
  default: return 44;
  }
}

static void test_switch(void)
{
  report("switch.dense",
         classify_dense(vi5) == 16 && classify_dense(vi2) == 7 &&
         classify_dense((int16_t)(vi5 * 3)) == -1);
  report("switch.sparse",
         classify_sparse(100) == 33 && classify_sparse(vi5) == 66 &&
         classify_sparse((int16_t)(vi5 + vi2)) == 22);
  report("switch.long",
         classify_long(vl1) == 11 && classify_long(vl2) == 33 &&
         classify_long(vl3) == 44);
}

// ---- function pointers (the VM dispatch pattern) ----------------------------

typedef uint32_t (*binop_t)(uint32_t, uint32_t);

static uint32_t fadd(uint32_t a, uint32_t b) { return a + b; }
static uint32_t fxor(uint32_t a, uint32_t b) { return a ^ b; }
static uint32_t fshl(uint32_t a, uint32_t b) { return a << (b & 31); }

static binop_t const ops[3] = {fadd, fxor, fshl};

static uint32_t apply(binop_t f, uint32_t a, uint32_t b) { return f(a, b); }

static void test_fptr(void)
{
  report32("fptr.table", ops[vop1](vA, vB2), 0x1234A987UL);
  report("fptr.arg", apply(ops[vop0], vA, vB2) == 0x12355677UL &&
                     apply(ops[vop2], vA, vs36) == 0x23456780UL);
}

// ---- setjmp/longjmp (MicroPython NLR fallback) ------------------------------

static jmp_buf jb;

static void deep3(void) { longjmp(jb, 9); }
static void deep2(void) { deep3(); }
static void deep1(void) { deep2(); }

// static volatile, NOT a local: Calypsi drops volatile stores to stack locals
// in functions like this one at every -O level (bug, see
// bugs/calypsi-volatile-auto-store-dropped.c and DECISIONS.md)
static volatile int16_t marker;

static void test_setjmp(void)
{
  marker = 0;
  int r = setjmp(jb);
  if (r == 0) {
    marker = 1;
    longjmp(jb, 7);
    report("setjmp.basic", 0);
  } else if (r == 7) {
    report("setjmp.basic", marker == 1);
    marker = 2;
    deep1(); // longjmps back with 9, across three frames
    report("setjmp.deep", 0);
  } else {
    report("setjmp.deep", r == 9 && marker == 2);
  }
  if (r != 9) {
    return; // only the final landing falls through to the caller
  }
}

// ---- varargs ----------------------------------------------------------------

static int16_t vsum_i(int n, ...)
{
  va_list ap;
  int16_t s = 0;
  va_start(ap, n);
  while (n--) {
    s += (int16_t)va_arg(ap, int);
  }
  va_end(ap);
  return s;
}

// In vsum_l/vsum_ll the 16-bit `n` (passed in the accumulator) is clobbered
// by the va_start/frame codegen before being saved — Calypsi bug, see
// bugs/calypsi-varargs-int-param-clobber.c. Workaround: copy it to a local
// before va_start, which forces a register save first. vsum_i needs no copy
// only by codegen luck; left as-is so a regression shows up here.

static int32_t vsum_l(int n, ...)
{
  va_list ap;
  int32_t s = 0;
  int count = n; // workaround, do not remove
  va_start(ap, n);
  while (count--) {
    s += va_arg(ap, long);
  }
  va_end(ap);
  return s;
}

static int64_t vsum_ll(int n, ...)
{
  va_list ap;
  int64_t s = 0;
  int count = n; // workaround, do not remove
  va_start(ap, n);
  while (count--) {
    s += va_arg(ap, long long);
  }
  va_end(ap);
  return s;
}

// the mp_printf shape: pointer as last named parameter, read after va_start
// (pointers are passed in pseudo registers, so the clobber bug does not bite)
static int32_t vsum_fmt(const char *fmt, ...)
{
  va_list ap;
  int32_t s = 0;
  va_start(ap, fmt);
  while (*fmt) {
    if (*fmt == 'l') {
      s += va_arg(ap, long);
    } else {
      s += va_arg(ap, int);
    }
    fmt++;
  }
  va_end(ap);
  return s;
}

static void test_varargs(void)
{
  report("va.int", vsum_i(4, 10, 20, 30, 40) == 100);
  report32("va.long", (uint32_t)vsum_l(3, 100000L, 200000L, 300000L), 600000UL);
  report64("va.longlong", (uint64_t)vsum_ll(2, 0x100000000LL, 0x23456789ALL),
           0x33456789AULL);
  report32("va.fmt", (uint32_t)vsum_fmt("lil", 100000L, 7, 200000L), 300007UL);
}

// ---- recursion --------------------------------------------------------------

static int16_t rec(int16_t n)
{
  volatile int16_t keep = n;
  if (n <= 0) {
    return 0;
  }
  return (int16_t)(keep + rec((int16_t)(n - 1)));
}

static void test_recursion(void)
{
  report("rec.depth32", rec(32) == 528);
}

// ---- C library --------------------------------------------------------------

static const char pat[] = "0123456789abcdefghijklmnopqrstuv";

static void test_libc(void)
{
  char buf[64];
  char dst[33];
  char src24[24], back24[24];
  int16_t i;
  int ok;

  memset(buf, 0xA5, sizeof buf);
  ok = 1;
  for (i = 0; i < 64; i++) {
    ok &= buf[i] == (char)0xA5;
  }
  memset(buf, 0, sizeof buf);
  for (i = 0; i < 64; i++) {
    ok &= buf[i] == 0;
  }
  report("libc.memset", ok);

  memcpy(dst, pat, sizeof pat);
  report("libc.memcpy", strcmp(dst, pat) == 0);

  report("libc.strcmp", strcmp("apple", "apple") == 0 &&
                        strcmp("apple", "apples") < 0 && strcmp("b", "a") > 0);

  // memcpy between banks: source on the bank-0 stack, destination in bank
  // $7F (in-bank object — far objects must not straddle a bank boundary)
  for (i = 0; i < 24; i++) {
    src24[i] = (char)(i * 7 + 3);
  }
  memcpy((void *)0x7F4000UL, src24, sizeof src24);
  ok = 1;
  for (i = 0; i < 24; i++) {
    ok &= *(volatile uint8_t *)(0x7F4000UL + i) == (uint8_t)(i * 7 + 3);
  }
  memcpy(back24, (const void *)0x7F4000UL, sizeof back24);
  for (i = 0; i < 24; i++) {
    ok &= back24[i] == src24[i];
  }
  report("libc.memcpyfar", ok);
}

// -----------------------------------------------------------------------------

int main(void)
{
  mb_init();
  mb_puts("M1 selftest\n");
  test_u32();
  test_i32();
  test_u64();
  test_i64();
  test_ptr();
  test_struct();
  test_switch();
  test_fptr();
  test_setjmp();
  test_varargs();
  test_recursion();
  test_libc();
  if (fails == 0) {
    mb_puts("M1 selftest passed\n");
    mb_finish(MB_STATUS_PASS);
  } else {
    mb_puts("M1 selftest FAILED\n");
    mb_finish(2);
  }
  return 0;
}
