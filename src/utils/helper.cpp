#include "sbi.h"
#include "libc.h"
#include "helper.h"

C void kputs(const char *s) {
  unsigned len = strlen(s);
  sbi_console_write(len, s);
}

C void kputch(char c) {
  sbi_console_write(1, &c);
}

C void panic(const char *s) {
  printk("Kernel panicked: %s\n", s);
  sbi_system_reset();
}
