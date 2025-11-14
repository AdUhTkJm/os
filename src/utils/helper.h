#ifndef HELPER_H
#define HELPER_H

#include "sbi.h"

#ifdef __cplusplus
#  define C extern "C"
#  include "helper.hpp"
#else
#  define C
#endif /* #ifdef __cplusplus */

/* Note that we're using clangd for IDE, but gcc for compilation. */
#ifdef __has_cpp_attribute
#  if __has_cpp_attribute(assume)
#     define assume(...) [[assume(__VA_ARGS__)]]
#  endif
#endif
#ifndef assume
#  if defined(__clang__)
#    define assume(...) __builtin_assume(__VA_ARGS__)
#  elif defined(__GNUC__) && __GNUC__ >= 13
#    define assume(...) __attribute__((__assume__(__VA_ARGS__)))
#  endif
#endif
#ifndef assume
#  define assume(...)
#endif

C void kputs(const char *s);
C void kputch(char c);

C [[noreturn]] void panic(const char *s);

C uint32_t rev_endian(uint32_t x);
C uint64_t rev_endian64(uint64_t x);

#define CSRW(reg, value) __asm__ volatile("csrw " #reg ", %0" :: "r"(value))
#define CSRR(reg, value) __asm__ volatile("csrr %0, " #reg : "=r"(value))
#define CSRS(reg, value) __asm__ volatile("csrs " #reg ", %0" :: "r"(value))
#define CSRC(reg, value) __asm__ volatile("csrc " #reg ", %0" :: "r"(value))

extern char __text_begin[], __text_end[];
extern char __rodata_begin[], __rodata_end[];
extern char __data_begin[], __data_end[];
extern char __bss_begin[], __bss_end[];
extern char __stack_top[], __kernel_end[];

extern reg_t rv_rdtime();

#endif
