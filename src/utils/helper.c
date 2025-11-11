#include "sbi.h"
#include "libc.h"

void kputs(const char *s) {
  unsigned len = strlen(s);
  sbi_console_write(len, s);
}

void kputch(char c) {
  sbi_console_write(1, &c);
}

void panic(const char *s) {
  kputs("kernel panicked: ");
  kputs(s);
  sbi_system_reset();
}
