#include <stdarg.h>
// variant 1: int count (the failing case)
long vsum_int(int n, ...) {
  va_list ap; long s = 0;
  va_start(ap, n);
  while (n--) s += va_arg(ap, long);
  va_end(ap);
  return s;
}
// variant 2: long count
long vsum_long(long n, ...) {
  va_list ap; long s = 0;
  va_start(ap, n);
  while (n--) s += va_arg(ap, long);
  va_end(ap);
  return s;
}
// variant 3: int count copied to local before va_start
long vsum_copy(int n, ...) {
  va_list ap; long s = 0;
  int count = n;
  va_start(ap, n);
  while (count--) s += va_arg(ap, long);
  va_end(ap);
  return s;
}
// variant 4: pointer last param, used after va_start (the mp_printf shape)
long vsum_fmt(const char *fmt, ...) {
  va_list ap; long s = 0;
  va_start(ap, fmt);
  while (*fmt++) s += va_arg(ap, long);
  va_end(ap);
  return s;
}
