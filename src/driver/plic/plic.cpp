#include "plic.h"
#include "../../utils/libc.h"
#include "../../mem/ptable.h"
#include "../../fs/devfs.h"

namespace os {

static_storage<ring_buffer<char>> console_input_buf;

// See https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc
void init_plic() {
  // Set priority of interrupt #10 to 1.
  *(volatile unsigned*) as_va(PLIC_BASE + UART0_IRQ * 4) = 1;
  // Enable interrupt #10.
  *(volatile unsigned*) as_va(PLIC_BASE + PLIC_ENABLE_S_OFFSET) |= (1 << UART0_IRQ);
  // Set priority threshold #10 (<= will be masked) to 0.
  *(volatile unsigned*) as_va(PLIC_BASE + PLIC_THRESHOLD_S_OFFSET) = 0;
  *(volatile unsigned char*) as_va(UART_BASE + UART_LCR) = 0x03;
  *(volatile unsigned char*) as_va(UART_BASE + 1) = 1;
  *(volatile unsigned char*) as_va(UART_BASE) = 0;
  
  devfs.construct();
  console.construct("console");
  console_input_buf.construct();
}

void handle_plic_interrupt() {
  // Claim the interrupt to get the IRQ ID.
  unsigned irq = *(volatile unsigned*) as_va(PLIC_BASE + PLIC_CLAIM_S_OFFSET);

  if (irq == UART0_IRQ) {
    // Read the register.
    char c = *(volatile unsigned char*) as_va(UART_BASE + UART_RBR);
    *(volatile unsigned*) as_va(PLIC_BASE + PLIC_CLAIM_S_OFFSET) = irq;
    console_input_buf->push_back(c);
    printk("received %c\n", c);
    console->wake();
    
    // Ctrl+C
    if (c == 0x03) {
      sbi_system_reset();
    }
  } else if (irq != 0) {
    *(volatile unsigned*) as_va(PLIC_BASE + PLIC_CLAIM_S_OFFSET) = irq;
  }
}

}
