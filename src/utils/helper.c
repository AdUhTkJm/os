#include "sbi.h"
#include "libc.h"

void kputs(const char *s) {
  unsigned len = strlen(s);
  sbicall(len, (reg_t) s, 0, 0, 0, 0, SBI_DBCN_CONSOLE_WRITE);
}

void kputch(char c) {
  sbicall(1, (reg_t) &c, 0, 0, 0, 0, SBI_DBCN_CONSOLE_WRITE);
}
