#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stdarg.h>

unsigned strlen(const char *s);
void *memset(void *p, int v, unsigned long size);
int printf(const char *fmt, ...);
char *itoa(long value, char *str, int base);

#endif
