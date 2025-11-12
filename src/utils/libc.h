#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned strlen(const char *s);
void *memset(void *p, int v, unsigned long size);
int printk(const char *fmt, ...);
char *itoa(long value, char *str, int base);

#ifdef __cplusplus
}
#endif

#endif
