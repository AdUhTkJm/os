#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../utils/plic.h"

void interrupt_handler(void *sp, reg_t scause, int stval, void *sepc) {
  if (scause < 0) {
    /* An interrupt. */
    int kind = scause & 0xff;
    switch (kind) {
    case 5:
      sbi_set_timer(rv_rdtime() + 5000000);
      break;
    case 9:
      handle_plic_interrupt();
      break;
    }
  }
}
