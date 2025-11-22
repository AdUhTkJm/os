#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../utils/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"

namespace {

using namespace os;

void on_miss(void *va) {
  EnableWriteToUserMemory enabler;

  printk("Page fault at: %p\n", va);
  auto *pcb = scheduler.active;

  va_t addr = (va_t) va;
  size_t i = 0;
  for (; i < pcb->vma.size(); i++) {
    const auto &vma = pcb->vma[i];
    if (addr <= vma.end && addr >= vma.begin)
      break;
  }
  if (i == pcb->vma.size()) {
    printk("Unmapped address %p. Terminate the process.\n", va);
    os::destruct(pcb);
    return;
  }
  auto pa = os::pframe();
  const auto &vma = pcb->vma[i];
  int flags = PTE_V | PTE_U | PTE_RWX;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;
  auto va_page = rounddown<4_kb>(va);
  os::pmap(pa, va_page, MAP_4KB, flags);

  // Copy the contents if it exists.
  if (!vma.backup)
    return;
  SeekGuard guard(vma.backup, vma.offset);
  vma.backup->read(va_page, PAGE_SIZE);
  printk("ksp = %p\n", pcb->ksp);
  printk("trapframe sepc = %p\n", ((trapframe*)pcb->ksp)->sepc);
}

void syscall(trapframe *ksp) {
  auto a7 = ksp->regs[15]; // a7 is x15.
  switch (a7) {
  default:
    printk("unknown syscall: %d\n", a7);
  }
  ksp->sepc += 4;
}

}

namespace os {

void interrupt_handler(reg_t scause, reg_t stval, void *sepc) {
  reg_t sstatus;
  CSRR(sstatus, sstatus);
  bool from_kernel = sstatus & (1 << 8);
  if (scause < 0) {
    int kind = scause & 0xff;
    switch (kind) {
    case 5: // Timer interrupt
      sbi_set_timer(rv_rdtime() + 3000000);
      switch_to(scheduler.choose());
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
    case 2: // Invalid instruction
      printk("exception (user): invalid instruction %p when executing %p\n", stval, sepc);
      destruct(scheduler.active);
      break;
    case 8: // System call
      syscall((trapframe *) scheduler.active->ksp);
      break;
    case 12: // Instruction page fault
    case 13: // Load page fault
    case 15: // Store page fault
      on_miss((void *) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
    }
  }
}

}
