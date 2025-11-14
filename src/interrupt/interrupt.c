#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../utils/plic.h"

void interrupt_handler(void *sp, reg_t scause, reg_t stval, void *sepc) {
  (void) sp;
  if (scause < 0) {
    /* An interrupt. */
    int kind = scause & 0xff;
    switch (kind) {
    case 5: /* Timer interrupt */
      sbi_set_timer(rv_rdtime() + 5000000);
      break;
    case 9: /* PLIC interrupt */
      handle_plic_interrupt();
      break;
    default:
      printk("interrupt: scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
      break;
    }
  } else {
    switch (scause) {
    case 5: /* Load access fault */
      printk("exception: load access fault at %p when executing %p\n", (void*) stval, sepc);
      break;
    case 7: /* Store access fault */
      printk("exception: store access fault at %p when executing %p\n", (void*) stval, sepc);
      break;
    case 13: /* Load page fault */
      printk("exception: load page fault at %p when executing %p\n", (void*) stval, sepc);
      break;
    default:
      printk("exception: scause = %ld, stval = %ld, sepc = %p\n", scause, stval, sepc);
      break;
    }
    panic("exception ocurred in kernel");
  }
}
