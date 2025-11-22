#ifndef PLIC_H
#define PLIC_H

#include "helper.h"
#include "../utils/stl/ring_buffer.h"

#define PLIC_BASE 0x0C000000
#define UART0_IRQ 10
#define UART_BASE 0x10000000
#define UART_IER_RDA 0x01
#define UART_RBR 0x0
#define UART_LCR 0x3

#define PLIC_ENABLE_S_OFFSET 0x2080
#define PLIC_THRESHOLD_S_OFFSET 0x201000
#define PLIC_CLAIM_S_OFFSET 0x201004

namespace os {

void init_plic();
void handle_plic_interrupt();

extern static_storage<ring_buffer<char>> console_input_buf;

}

#endif
