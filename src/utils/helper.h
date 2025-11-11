#ifndef HELPER_H
#define HELPER_H

#include "sbi.h"

#ifdef __cplusplus
#define C extern "C"

namespace os {

// TODO: Basic STL

}
#else
#define C
#endif // #ifdef __cplusplus

C void kputs(const char *s);
C void kputch(char c);

C [[noreturn]] void panic(const char *s);

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
