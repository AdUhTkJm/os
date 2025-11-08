#include <stdint.h>
#include "../utils/libc.h"
#include "../utils/plic.h"

extern char *__bss_begin, *__bss_end;

void kernel_main() {
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  printf("Kernel launched\n");
  init_plic();
  printf("PLIC enabled.\n");
  for (;;) ;
}
