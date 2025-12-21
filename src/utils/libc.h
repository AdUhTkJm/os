#ifndef LIBC_H
#define LIBC_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#define __assert_dump_stack
#ifdef __cplusplus
#  ifdef FUNC_INSTRUMENT
#  undef __assert_dump_stack
namespace os::stack { void dump(); }
#  define __assert_dump_stack stack::dump();
#  endif
extern "C" {
#endif


[[noreturn]] void panic(const char *s);
#define __assert0(x, line) do { \
  [[unlikely]] if (!(x)) { \
    __assert_dump_stack; \
    panic(__FILE__ ":" #line ": assertion failed: " #x); \
  } \
} while (0)
#define __assert1(x, line) __assert0(x, line)
#define assert(x) __assert1(x, __LINE__)

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
