#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

unsigned strlen(const char *s);
void *memset(void *p, int v, size_t size);
void *memcpy(void *dst, const void *src, size_t n);
int printk(const char *fmt, ...);
char *itoa(int value, char *str, int base);
char *ltoa(long value, char *str, int base);
char *ultoa(unsigned long value, char *str, int base);
int atoi(const char *str);
long atol(const char *str);
unsigned long atoul(const char *str);
unsigned long hextoul(const char *str);
int isspace(int c);
int strcmp(const char *l, const char *r);
int memcmp(const void *l, const void *r, size_t n);
int strncmp(const char *l, const char *r, size_t n);
void strcpy(char *dst, const char *src);
void strcat(char *dst, const char *src);
unsigned long strtoul(const char *s, char **endptr, int base);
const char *strstr(const char *haystack, const char *needle);
void srand(unsigned);
size_t __rand64();
int rand();

#ifdef __cplusplus
}
#endif

#endif
