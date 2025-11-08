#ifndef HELPER_H
#define HELPER_H

#include "sbi.h"

void kputs(const char *s);
void kputch(char c);

#define CSRW(reg, value) __asm__ volatile("csrw " #reg ", %0" :: "r"(value))
#define CSRR(reg, value) __asm__ volatile("csrr %0, " #reg : "=r"(value))
#define CSRS(reg, value) __asm__ volatile("csrs " #reg ", %0" :: "r"(value))
#define CSRC(reg, value) __asm__ volatile("csrc " #reg ", %0" :: "r"(value))

extern reg_t rv_rdtime();

#endif
