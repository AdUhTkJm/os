#include "plic.h"
#include "../../utils/libc.h"
#include "../../mem/ptable.h"
#include "../../fs/devfs.h"

namespace {
  
[[gnu::no_instrument_function]] void console_input_handler(int irq) {
  // Read the register.
  while (/*data ready bit*/os::mmrd<uint8_t>(UART_BASE + UART_LSR) & 0x01) {
    char c = os::mmrd<char>(UART_BASE + UART_RBR);
    
    // Ctrl+C, for debugging (early exit)
    if (c == 0x03)
      sbi_system_reset();

    os::console_input_buf->push_back(c);
  }
  os::mmwr(PLIC_BASE + PLIC_CLAIM_S_OFFSET, irq);
  os::console->wake_read(); // Note that this function might not return.
}

}

namespace os {  

static_storage<ring_buffer<char>> console_input_buf;

}

namespace os::plic {

static_storage<hashmap<int, void(*)(int)>> handlers;

void enable(int device) {
  // Set priority of interrupt #10 to 1.
  mmwr(PLIC_BASE + device * 4, 1u);
  // Enable interrupt #10.
  // We must access this on 4-byte boundary, but note that the argument of as_va is byte-sized.
  *(volatile unsigned*) as_va(PLIC_BASE + PLIC_ENABLE_S_OFFSET + 4 * (device / 32)) |= (1 << device % 32);
}

// See https://github.com/riscv/riscv-plic-spec/blob/master/riscv-plic.adoc
void init() {
  enable(UART0_IRQ);
  // Set priority threshold of the hart to 0. (priority <= 0 will be masked)
  mmwr(PLIC_BASE + PLIC_THRESHOLD_S_OFFSET, 0u);

  // Also set up the UART device here - able to debug as soon as possible.
  mmwr<unsigned char>(UART_BASE + UART_LCR, 0x03);
  mmwr<unsigned char>(UART_BASE + 1, 1);
  mmwr<unsigned char>(UART_BASE, 0);
  
  devfs.construct();
  console.construct();
  console_input_buf.construct();
  handlers.construct();

  record(UART0_IRQ, console_input_handler);
}

[[gnu::no_instrument_function]] void handle() {
  // Claim the interrupt to get the IRQ ID.
  unsigned irq = mmrd<unsigned>(PLIC_BASE + PLIC_CLAIM_S_OFFSET);
  if (handlers->count(irq))
    handlers->at(irq)(irq);
  else
    mmwr(PLIC_BASE + PLIC_CLAIM_S_OFFSET, irq);
}

void record(int device, void(*handler)(int)) {
  (*handlers)[device] = handler;
}

}
