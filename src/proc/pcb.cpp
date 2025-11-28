#include "elf.h"
#include "pcb.h"
#include "schedule.h"
#include "../mem/kalloc.h"

namespace os {

static_storage<hashmap<int, pcb_t*>> pid_map_s;

int process_file_table::allocate(file *f, int fd) {
  // A file descriptor number is specified.
  if (fd != -1) {
    deallocate(fd);
    open[fd] = f;
    desc[fd] = 0;
    return fd;
  }

  // Find a usable descriptor.
  for (int i = 3; ; i++) {
    if (!open.count(i)) {
      open[i] = f;
      desc[fd] = 0;
      f->refcnt++;
      return i;
    }
  }
}

void process_file_table::deallocate(int fd) {
  if (!open.count(fd))
    return;
  open[fd]->close();
  open.erase(fd);
  desc.erase(fd);
}

void process_file_table::clear() {
  for (auto [_, f] : open) {
    f->close();
  }
}

void pcb_t::clear() {
  pt::free(pt_root);
  pt_root = (pa_t) kernel_pt_root - KERNEL_OFFSET;
  ftbl.clear();
  status = Zombie;
}

int nextpid() {
  static spinlock lock;
  static int pid = 0;
  synchronized syn(lock);
  return pid++;
}

void init_user(pcb_t *pcb) {
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
}

void init(pcb_t *pcb) {
  // Gives one physical page for the root page table and the kernel stack.
  pcb->pt_root = pframe();
  pcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb->pt_root), kernel_pt_root, PAGE_SIZE);

  // Open stdin, stdout and stderr.
  // Note they are different files, but point to the same place.
  auto console = vfs->lookup("/dev/console");
  if (!console)
    panic("no console!");
  for (int i = 0; i < 3; i++)
    pcb->ftbl.allocate(new file(console->node, O_RDWR), i);

  pcb->ctx_valid = false;
}

void terminate(pcb_t *pcb, int ret) {
  // This will dispatch.
  scheduler.erase(pcb);
  pcb->clear();
  pcb->ret = ret;
  // Change all child processes to children of init.
  // It is expected that init will recycle them later.
  auto init = (*pid_map_s)[1];
  for (auto child : pcb->children) {
    child.parent = init;
    init->children.push_back(&child);
  }
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
  if (!pcb->kproc)
    sstatus = (sstatus & ~(1 << 8));
  trap->sstatus = sstatus | (1 << 5);
  CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
  pt_root = (pte_t *) as_va(pcb->pt_root);
}

void trap_return_setup(pcb_t *pcb) {
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
  pcb->children.push_back(child);

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

  // Shallow-copy the file table.
  child->ftbl = pcb->ftbl;
  for (auto [_, f] : child->ftbl)
    f->refcnt++;

  child->status = Ready;
  child->vma = pcb->vma;
  scheduler.add(child);
  return child->pid;
}

result exec(const string &path, char *const *argv, char *const *envp) {
  auto pcb = scheduler.active;
  pt::free(pcb->pt_root);
  pfree(pcb->ksp);
  
  File file(path, O_RDONLY);
  if (load_elf(file, pcb) != result::success)
    return result::failure;

  // Close files according to flags.
  os::vector<int> toclose;
  for (auto [fd, f] : pcb->ftbl) {
    if (pcb->ftbl.get_desc(fd) & FD_CLOEXEC
     || f->flags & O_CLOEXEC)
      toclose.push_back(fd);
  }
  for (auto fd : toclose)
    pcb->ftbl.deallocate(fd);

  (void) argv; (void) envp;
  // Yield. On next schedule, the trap frame would be set up properly.
  scheduler.add(pcb);
  scheduler.dispatch();
  return result::success;
}

}
