#ifndef HELPER_H
#define HELPER_H

#include "sbi.h"
#include "errorcode.h"

#if !defined(__clang__) && !defined(__GNUC__)
#error This OS kernel must be compiled with clang or GNUC.
#endif

#ifdef __cplusplus
#  define C extern "C" [[gnu::no_instrument_function]] 
// #include must be together for the build script to detect dependencies.
#include "helper.hpp"
#else
#  define C
#endif

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

#define __assert0(x, line) do { [[unlikely]] if (!(x)) panic(__FILE__ ":" #line ": assertion failed: " #x); } while (0)
#define __assert1(x, line) __assert0(x, line)
#define assert(x) __assert1(x, __LINE__)

/* I always don't remember the exact form of test macros, so redefine them here. */
/* This also helps VSCode to highlight - just change position of IN_VSCODE. */
#if defined(__riscv) || IN_VSCODE
#  define RV
#elif defined(__loongarch__)
#  define LA
#else
#  error Unknown architecture.
#endif

typedef long ssize_t;

C void kputs(const char *s);
C void kputch(char c);

C [[noreturn]] void panic(const char *s);

C uint32_t to_big_endian(uint32_t x);
C uint64_t to_big_endian64(uint64_t x);

C void hexdump(const void *ptr, size_t len);

#define CSRW(reg, value) __asm__ volatile("csrw " #reg ", %0" :: "r"(value))
#define CSRR(reg, value) __asm__ volatile("csrr %0, " #reg : "=r"(value))
#define CSRS(reg, value) __asm__ volatile("csrs " #reg ", %0" :: "r"(value))
#define CSRC(reg, value) __asm__ volatile("csrc " #reg ", %0" :: "r"(value))

#define MV(reg, value) __asm__ volatile("mv " #reg ", %0" :: "r"(value))
#define RD(reg, value) __asm__ volatile("mv %0, " #reg : "=r"(value))

extern char __text_begin[], __text_end[];
extern char __rodata_begin[], __rodata_end[];
extern char __data_begin[], __data_end[];
extern char __bss_begin[], __bss_end[];
extern char __stack_top[];
extern char __kernel_begin[], __kernel_end[];

extern reg_t rdtime();

#endif
