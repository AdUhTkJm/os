#include "plic.h"
#include "libc.h"
#include "helper.h"

// See https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc

void os::init_plic() {
  // Set priority of interrupt #10 to 1.
  *(volatile unsigned*)(PLIC_BASE + UART0_IRQ * 4) = 1;
  // Enable interrupt #10.
  *(volatile unsigned*)(PLIC_BASE + PLIC_ENABLE_S_OFFSET) |= (1 << UART0_IRQ);
  // Set priority threshold #10 (<= will be masked) to 0.
  *(volatile unsigned*)(PLIC_BASE + PLIC_THRESHOLD_S_OFFSET) = 0;
  *(volatile unsigned char*)(UART_BASE + UART_LCR) = 0x03;
  *(volatile unsigned char*)(UART_BASE + 1) = 1;
  *(volatile unsigned char*)(UART_BASE) = 0;
}

void os::handle_plic_interrupt() {
  // Claim the interrupt to get the IRQ ID.
  unsigned irq = *(volatile unsigned*)(PLIC_BASE + PLIC_CLAIM_S_OFFSET);

  if (irq == UART0_IRQ) {
    // Read the register.
    char c = *(volatile unsigned char*)(UART_BASE + UART_RBR);
    *(volatile unsigned*)(PLIC_BASE + PLIC_CLAIM_S_OFFSET) = irq;
    
    // Ctrl+C
    if (c == 0x03) {
      sbi_system_reset();
    }
  } else if (irq != 0) {
    *(volatile unsigned*)(PLIC_BASE + PLIC_CLAIM_S_OFFSET) = irq;
  }
}
