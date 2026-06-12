#include <setjmp.h>
static jmp_buf jb;
extern void sink(int v);
extern void deep1(void);
static void probe(void) {
  volatile int m = 0;
  int r = setjmp(jb);
  if (r == 0) {
    m = 1;
    longjmp(jb, 7);
    sink(0);              // dead code after longjmp, as in M1
  } else if (r == 7) {
    sink(m == 1);
    m = 2;
    deep1();
    sink(0);
  } else {
    sink(r == 9 && m == 2);
  }
}
void run(void) { probe(); }
