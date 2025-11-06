#include <stdint.h>
#include "../utils/sbi.h"
#include "../utils/libc.h"

// extern char *__bss, *__bss_end;

void kputs(const char *s) {
  unsigned len = strlen(s);
  sbicall(len, ((reg_t) s), 0, 0, 0, 0, SBI_DBCN_CONSOLE_WRITE);
}

void kernel_main() {
  sbicall('A', 0, 0, 0, 0, 0, 0, 1);
  // memset(__bss, 0, __bss_end - __bss);
  const char s[] = "Hello World\n";
  kputs(s);
  for (;;) ;
}
