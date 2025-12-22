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
#  define C extern "C" [[gnu::no_instrument_function]]
#endif


C [[noreturn]] void panic(const char *s);
#define __assert0(x, line) do { \
  [[unlikely]] if (!(x)) { \
    __assert_dump_stack; \
    panic(__FILE__ ":" #line ": assertion failed: " #x); \
  } \
} while (0)
#define __assert1(x, line) __assert0(x, line)
#define assert(x) __assert1(x, __LINE__)

C unsigned strlen(const char *s);
C void *memset(void *p, int v, size_t size);
C void *memcpy(void *dst, const void *src, size_t n);
C int printk(const char *fmt, ...);
C char *itoa(int value, char *str, int base);
C char *ltoa(long value, char *str, int base);
C char *ultoa(unsigned long value, char *str, int base);
C int atoi(const char *str);
C long atol(const char *str);
C unsigned long atoul(const char *str);
C unsigned long hextoul(const char *str);
C int isspace(int c);
C int strcmp(const char *l, const char *r);
C int memcmp(const void *l, const void *r, size_t n);
C int strncmp(const char *l, const char *r, size_t n);
C void strcpy(char *dst, const char *src);
C void strcat(char *dst, const char *src);
C unsigned long strtoul(const char *s, char **endptr, int base);
C const char *strstr(const char *haystack, const char *needle);
C void srand(unsigned);
C size_t __rand64();
C int rand();

#endif
