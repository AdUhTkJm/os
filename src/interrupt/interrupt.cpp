#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../utils/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"

namespace {

using namespace os;

void on_load_miss(void *va) {
  printk("Load miss: %p", va);
  auto pa = os::pframe();
  auto &pcb = *scheduler.get_active();
  size_t i = 0;
  for (; i < pcb.vma.size(); i++) {
    const auto &vma = pcb.vma[i];
    if (pa <= vma.end && pa >= vma.begin)
      break;
  }
  if (i == pcb.vma.size()) {
    printk("Unmapped address %p. Terminate the process.\n", va);
    os::destruct(&pcb);
    return;
  }
  const auto &vma = pcb.vma[i];
  int flags = PTE_V;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;
  os::pmap(pa, (va_t) va, MAP_4KB, flags);
}

}

namespace os {

void interrupt_handler(void *sp, reg_t scause, reg_t stval, void *sepc) {
  (void) sp;
  reg_t sstatus;
  CSRR(sstatus, sstatus);
  bool from_kernel = sstatus & (1 << 8);
  if (scause < 0) {
    int kind = scause & 0xff;
    switch (kind) {
    case 5: // Timer interrupt
      sbi_set_timer(rv_rdtime() + 5000000);
      switch_to(scheduler.choose(), sp);
      break;
    case 9: // PLIC interrupt
      os::handle_plic_interrupt();
      break;
    default:
      printk("interrupt: scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
      break;
    }
  } else if (from_kernel) {
    switch (scause) {
    case 5: // Load access fault
      printk("exception: load access fault at %p when executing %p\n", stval, sepc);
      break;
    case 7: // Store access fault
      printk("exception: store access fault at %p when executing %p\n", stval, sepc);
      break;
    case 12: // Instruction page fault
      printk("exception: instruction page fault at %p when executing %p\n", stval, sepc);
      break;
    case 13: // Load page fault
      printk("exception: load page fault at %p when executing %p\n", stval, sepc);
      break;
    case 15: // Store page fault
      printk("exception: store page fault at %p when executing %p\n", stval, sepc);
      break;
    default:
      printk("exception: scause = %ld, stval = %ld, sepc = %p\n", scause, stval, sepc);
      break;
    }
    panic("exception occurred in kernel");
  } else {
    switch (scause) {
    case 13:
      on_load_miss((void *) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
    }
  }
}

}
