#include <stdint.h>
#include "../utils/libc.h"

extern char *__bss_begin, *__bss_end;

#define PLIC_BASE 0x0C000000
#define UART0_IRQ 10
#define UART_BASE 0x10000000
#define UART_IER_RDA 0x01

// Enable UART0 IRQ for hart 0

void kernel_main() {
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  printf("Kernel launched.\n");
  // __asm__ volatile("unimp");
  *(volatile unsigned*)(PLIC_BASE + UART0_IRQ * 4) = 1;
  *(volatile unsigned*)(PLIC_BASE + 0x2000) |= (1 << UART0_IRQ);
  *(volatile unsigned*)(PLIC_BASE + 0x200000) = 0;
  *(volatile unsigned char*)(UART_BASE + 1) |= UART_IER_RDA;
  printf("PLIC enabled.\n");
  for (;;) ;
}
