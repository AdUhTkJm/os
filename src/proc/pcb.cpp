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

void init(pcb_t *pcb_p) {
  auto &pcb = *pcb_p;
  static int pid = 1;
  pcb.pid = pid++;
  
  va_t max = 0;
  for (const auto &vma : pcb.vma)
    max = os::max(vma.end, max);
  va_t heap_start = os::roundup<PAGE_SIZE>(max);

  // Allocate a stack.
  pcb.vma.push_back({
    .begin = stack_top, .end = stack_size, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });
  // Allocate a heap. It is initially quite small.
  pcb.vma.push_back({
    .begin = heap_start, .end = heap_start + PAGE_SIZE, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });

  pcb.pt_root = pframe();

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb.pt_root), kernel_pt_root, PAGE_SIZE);
  pcb.status = Ready;
  scheduler.add(pcb_p);
}

void destruct(pcb_t *pcb) {
  scheduler.erase(pcb);
  delete pcb;
}

[[noreturn]] void activate(pcb_t *pcb_p, int argc, char **argv, char **envp) {
  auto &pcb = *pcb_p;

  CSRW(sepc, pcb.entry);
  CSRC(sstatus, 1 << 8); // SPP = 0: user process
  CSRS(sstatus, 1 << 5); // SPIE = 1: interrupt enabled
  CSRW(satp, SATP_MODE_SV39 | (pcb.pt_root >> 12));
  // TODO: copy the strings to the stack.
/*
  MV(sp, pcb.sp);
  uint64_t *sp = (uint64_t *) pcb.sp;

  // Copy the address of envp strings.
  *--sp = 0;
  for (int i = argc - 1; i >= 0; --i)
    *--sp = (uint64_t) envp[i];
  
  // Copy the address of argv strings.
  *--sp = 0;
  for (int i = argc - 1; i >= 0; --i)
    *--sp = (uint64_t) argv[i];

  *--sp = argc;
  pcb.sp = (va_t) sp;
*/
  __asm__ volatile("jr %0" :: "r"(pcb.entry));
  __builtin_unreachable();
}

[[noreturn]] void context_restore(reg_t sp);
void switch_to(pcb_t *pcb_p, void *sp) {
  pcb_t &active = *scheduler.get_active();
  active.sp = (va_t) sp;

  pcb_t &pcb = *pcb_p;
  CSRW(satp, SATP_MODE_SV39 | (pcb.pt_root >> 12));
  context_restore(pcb.sp);
}

}
