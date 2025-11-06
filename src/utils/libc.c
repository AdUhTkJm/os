#include "libc.h"

unsigned strlen(const char *s) {
  unsigned result = 0;
  while (*s++)
    result++;
  return result;
}

void *memset(void *p, int v, unsigned long size) {
  while (size--)
    *(char*)p++ = v;
  return p;
}
