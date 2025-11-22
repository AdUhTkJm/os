#include "pcb.h"
#include "schedule.h"
#include "../mem/kalloc.h"

namespace os {

int process_file_table::allocate(file *f) {
  for (int i = 3; ; i++) {
    if (!open.count(i)) {
      open[i] = f;
      f->refcnt++;
      return i;
    }
  }
}

void process_file_table::deallocate(int fd) {
  if (!open.count(fd))
    return;
  open[fd]->refcnt--;
  open.erase(fd);
}

void init(pcb_t *pcb) {
  static int pid = 1;
  pcb->pid = pid++;
  
  va_t max = 0;
  for (const auto &vma : pcb->vma)
    max = os::max(vma.end, max);
  va_t heap_start = os::roundup<PAGE_SIZE>(max);

  // Allocate a heap. It is initially quite small.
  pcb->vma.push_back({
    .begin = heap_start, .end = heap_start + PAGE_SIZE, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });
  // Allocate a stack.
  pcb->vma.push_back({
    .begin = stack_top, .end = stack_top + user_stack_size, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });

  // Gives one physical page for the root page table and the kernel stack.
  pcb->pt_root = pframe();
  pcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb->pt_root), kernel_pt_root, PAGE_SIZE);
  scheduler.add(pcb);
}

void terminate(pcb_t *pcb, int ret) {
  scheduler.erase(pcb);
  pcb->ksp = Zombie;
  pcb->ret = ret;
}

static void first_time_setup(pcb_t *pcb) {
  pcb->status = Running;
  // Construct a trap frame on the kernel stack.
  // Note that stack grows downwards, so we self-decrement
  // and leave the space for it.
  auto trap = (trapframe *) pcb->ksp;
  trap->sepc = pcb->pc;
  int sstatus; CSRR(sstatus, sstatus);
  // User process with interrupt enabled.
  trap->sstatus = (sstatus & ~(1 << 8)) | (1 << 5);
  CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
  pt_root = (pte_t *) as_va(pcb->pt_root);
}

void trap_return_setup(pcb_t *pcb) {
  pcb_t *active = scheduler.active;
  CSRR(sepc, active->pc);
  scheduler.active = pcb;

  [[unlikely]] if (pcb->status == Init) {
    first_time_setup(pcb);
    return;
  }

  pcb->status = Running;
  CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
  pt_root = (pte_t *) as_va(pcb->pt_root);
  auto trap = (trapframe *) pcb->ksp;
  trap->sepc = pcb->pc;
}

}
