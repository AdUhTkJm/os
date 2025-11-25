#include "pcb.h"
#include "schedule.h"
#include "../mem/kalloc.h"

namespace os {

int process_file_table::allocate(file *f, int fd) {
  // A file descriptor number is specified.
  if (fd != -1) {
    deallocate(fd);
    open[fd] = f;
    return fd;
  }

  // Find a usable descriptor.
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
  if (!--open[fd]->refcnt) {
    delete open[fd];
    open.erase(fd);
  }
}

int nextpid() {
  static spinlock lock;
  static int pid = 1;
  synchronized syn(lock);
  return pid++;
}

void init(pcb_t *pcb) {
  pcb->pid = nextpid();
  
  va_t max = 0;
  for (const auto &vma : pcb->vma)
    max = os::max(vma.end, max);
  va_t heap_start = os::roundup<PAGE_SIZE>(max);

  // Allocate a heap. It is initially quite small.
  pcb->vma.push_back({
    .begin = heap_start, .end = heap_start + PAGE_SIZE, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });
  // Allocate a stack. Note it grows downwards.
  pcb->vma.push_back({
    .begin = stack_top - user_stack_size, .end = stack_top, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE, .backup = nullptr, .offset = 0
  });

  // Gives one physical page for the root page table and the kernel stack.
  pcb->pt_root = pframe();
  pcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb->pt_root), kernel_pt_root, PAGE_SIZE);

  // Open stdin, stdout and stderr.
  // Note they are different files, but point to the same place.
  auto tty0 = vfs->lookup("/dev/tty0");
  if (!tty0)
    panic("no console!");
  for (int i = 0; i < 3; i++)
    pcb->ftbl.allocate(new file(tty0->node, O_RDWR), i);

  scheduler.add(pcb);
}

void terminate(pcb_t *pcb, int ret) {
  // Currently, without the idle process, the termination would fail.
  panic("terminate: not yet implemented");
  
  scheduler.erase(pcb);
  pcb->ret = ret;
}

static void first_time_setup(pcb_t *pcb) {
  pcb->status = Running;
  // Construct a trap frame on the kernel stack.
  // Note that stack grows downwards, so we self-decrement
  // and leave the space for it.
  auto trap = (trapframe *) pcb->ksp;
  trap->sepc = pcb->pc;
  // Let sp point to the user stack.
  trap->sscratch = stack_top;

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

int fork() {
  auto pcb = scheduler.active;
  auto child = new pcb_t;
  child->parent = pcb;

  // Allocate a new kernel stack.
  child->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  memcpy((char *) child->ksp, (char *) pcb->ksp, sizeof(trapframe));

  // Set the return value (a0) of child to zero.
  auto trap = (trapframe *) child->ksp;
  trap->regs[8] = 0;
  child->pc = trap->sepc;

  child->pid = nextpid();
  
  TLBRefreshGuard guard;

  // Mark the parent's table as copy-on-write.
  pt::walk((pte_t *) as_va(pcb->pt_root), [](pte_t &pte) {
    // Only do this on user pages that are writable.
    if (!(pte & PTE_U) || !(pte & PTE_W))
      return;
    
    pte &= ~PTE_W;
    pte |= PTE_COW;
  });

  // Deep-copy the page table.
  assert(pt_root == (pte_t *) as_va(pcb->pt_root));
  child->pt_root = pt::copy(pt_root);
  printk("pa = %p in parent\n", to_pa(0x100b4));
  auto old = pt_root;
  pt_root = (pte_t*) as_va(child->pt_root);
  pt::walk(pt_root, [](pte_t pte) {
    if (pte & PTE_U)
      printk("child pte = %p\n", pte);
  });
  printk("pa = %p in child\n", to_pa(0x100b4));
  pt_root = old;

  // Shallow-copy the file table.
  child->ftbl = pcb->ftbl;
  for (auto [_, f] : child->ftbl)
    f->refcnt++;

  child->status = Ready;
  child->vma = pcb->vma;
  scheduler.add(child);
  return child->pid;
}

}
