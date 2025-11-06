#ifndef HELPER_H
#define HELPER_H

void kputs(const char *s);
void kputch(char c);

#define CSRW(reg, value) __asm__ volatile("csrw " #reg ", %0" :: "r"(value))
#define CSRR(reg, value) __asm__ volatile("csrr %0, " #reg : "=r"(value))

#endif
