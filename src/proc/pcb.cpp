#include "pcb.h"
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

void init(pcb_t &pcb) {
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

  // Map the kernel region.
  for (int i = 0; i < 64; i++)
    pmap((pa_t) __kernel_begin + i * 2_mb, (va_t) __kernel_begin + i * 2_mb, MAP_2MB, PTE_RWX | PTE_V);
}

void destruct(pcb_t &pcb) {
  (void) pcb;
}

void activate(pcb_t &pcb) {
  CSRW(sepc, pcb.entry);
  CSRC(sstatus, 1 << 8); // SPP = 0: user process
  CSRS(sstatus, 1 << 5); // SPIE = 1: interrupt enabled

  MV(sp, stack_top);
  // TODO: argc, argv, envp
}

}
