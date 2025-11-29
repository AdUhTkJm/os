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
      f->ref();
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
  for (const auto &vma : this->vma) {
    if (vma.backup)
      vma.backup->drop();
  }
}

int pcb_t::open_file(const string &path, int flags) {
  bool create = flags & O_CREAT;
  bool existok = !(flags & O_EXCL);
  bool write = bool(flags & O_RDWR) || bool(flags & O_WRONLY);
  bool read = bool(flags & O_RDWR) || bool(flags & O_RDONLY);

  // TODO: check search permission
  auto maybe_dentry = vfs->lookup(path);
  if (!maybe_dentry) {
    if (!create)
      return maybe_dentry;

    auto parent = dirname(path);
    auto maybe_parent = vfs->lookup(parent);
    if (!maybe_parent)
      return maybe_parent;

    auto node = (*maybe_parent)->node;
    if (int err = node->create(basename(path), inode::File); err != 0)
      return err;

    return open_file(path, flags & ~O_CREAT);
  }

  if (create && !existok)
    return -EEXIST;

  auto dentry = *maybe_dentry;
  inode *node = dentry->node;
  if (node->type == inode::Dir && write)
    return -EISDIR;

  // Check the flags of node.
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  if (uid != 0) {
    // Skip check for root.

    if (read && !(flags & (1 << bit + 2)))
      return -EACCES;
    
    if (write && !(flags & (1 << bit + 1)))
      return -EACCES; 
  }

  file *f = new file(node, flags);
  int fd = ftbl.allocate(f);
  return fd;
}

int pcb_t::close_file(int fd) {
  if (!ftbl.count(fd))
    return -EINVAL;
  
  ftbl.deallocate(fd);
  return 0;
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
    .begin = stack_top - user_stack_size, .end = pcb->usp = stack_top, .prot = PROT_READ | PROT_WRITE,
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
  pcb->ftbl.allocate(new file((*console)->node, O_RDONLY), 0); // stdin
  pcb->ftbl.allocate(new file((*console)->node, O_WRONLY), 1); // stdout
  pcb->ftbl.allocate(new file((*console)->node, O_WRONLY), 2); // stderr
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
  trap->sscratch = pcb->usp;

  int sstatus; CSRR(sstatus, sstatus);
  // User process with interrupt enabled.
  if (!pcb->kproc)
    sstatus = (sstatus & ~(1 << 8));
  else
    sstatus = (sstatus | (1 << 8));
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
    f->ref();

  // Copy various information from parent.
  child->status = Ready;
  child->vma = pcb->vma;
  child->kproc = pcb->kproc;
  child->uid = pcb->uid;
  child->gid = pcb->gid;
  scheduler.add(child);
  return child->pid;
}

class File {
  int fd;
public:
  File(const string &path, int flags) {
    auto pcb = scheduler.active;
    fd = pcb->open_file(path, flags);
  }
  ~File() {
    auto pcb = scheduler.active;
    pcb->close_file(fd);
  }
  operator file*() const {
    auto pcb = scheduler.active;
    return pcb->ftbl[fd];
  }
};

/*
+---------------------------+  <-- usp
| argc                      |
+---------------------------+
| argv[0] (pointer)         |
| argv[1] (pointer)         |
| ...                       |
| argv[argc] = NULL         |
+---------------------------+
| envp[0] (pointer)         |
| envp[1] (pointer)         |
| ...                       |
| envp[n] = NULL            |
+---------------------------+
| AT_* auxiliary vectors    |
| (pairs of (type, value))  |
| ...                       |
| AT_NULL = 0               |
+---------------------------+
| argument strings          |
| environment strings       |
+---------------------------+
*/
int exec(const string &path, char *const *argv, char *const *envp) {
  auto pcb = scheduler.active;
  pt::free(pcb->pt_root);
  pfree(pcb->ksp);
  pcb->vma.clear();
  
  // This performs the initialization of pcb.
  int fd = pcb->open_file(path, O_RDONLY);
  if (auto ret = load_elf(pcb->ftbl[fd], pcb); ret != 0)
    return ret;

  // Close files according to flags.
  os::vector<int> toclose;
  for (auto [fd, f] : pcb->ftbl) {
    if (pcb->ftbl.get_desc(fd) & FD_CLOEXEC
     || f->flags & O_CLOEXEC)
      toclose.push_back(fd);
  }
  for (auto fd : toclose)
    pcb->ftbl.deallocate(fd);

  char *usp = (char *) pcb->usp;
  os::vector<char*> argvp, envpp;
  // Copy the real contents of the strings.
  for (char *const *p = envp; *p; p++) {
    char *str = *p;
    int len = strlen(str);
    copy_to_user(usp -= len, str, len, pcb);
    envpp.push_back(usp);
  }
  for (char *const *p = argv; *p; p++) {
    char *str = *p;
    int len = strlen(str);
    copy_to_user(usp -= len, str, len, pcb);
    argvp.push_back(usp);
  }

  // Copy the pointers.
  // We copy envp pointers first, so that argv will be closer to stack top,
  // as required by the ABI.
  *(uintptr_t*) (usp -= 8) = 0;
  for (int i = int(envpp.size()); i >= 0; i--) {
    auto ptr = envpp[i];
    copy_to_user(usp -= 8, &ptr, 8, pcb);
  }

  *(uintptr_t*) (usp -= 8) = 0;
  for (int i = int(argvp.size()); i >= 0; i--) {
    auto ptr = argvp[i];
    copy_to_user(usp -= 8, &ptr, 8, pcb);
  }

  int argc = argvp.size();
  copy_to_user(usp -= 4, &argc, 4, pcb);
  pcb->usp = (va_t) usp;

  scheduler.add(pcb);
  return 0;
}

void copy_to_user(void *usr, void *ker, size_t len, pcb_t *pcb) {
  EnableAccessToUserMemory enable;
  vma_map(usr, (char*) usr + len, pcb);
  memcpy(usr, ker, len);
}

void copy_to_user(void *usr, void *ker, size_t len) {
  copy_to_user(usr, ker, len, scheduler.active);
}

errable<char*> copy_from_user(void *usr, size_t len) {
  EnableAccessToUserMemory enable;
  vma_map_current(usr, (char *) usr + len);
  char *buf = new char[len];
  memcpy(buf, usr, len);
  return buf;
}

errable<char*> copy_from_user(void *usr) {
  EnableAccessToUserMemory enable;
  vma_map_current(usr);
  vector<char> vec;
  char *p = (char *) usr;
  for (; p < roundup<PAGE_SIZE>(usr) && *p; p++)
    vec.push_back(*p);

  for (int i = 0; i < 4096; i++) {
    vma_map_current(p);
    for (char *finish = p + PAGE_SIZE; p < finish && *p; p++)
      vec.push_back(*p);
    
    if (!*p)
      goto outer;
  }
  return -E2BIG;
outer:
  int sz = vec.size();
  char *buf = new char[1 + sz];
  memcpy(buf, vec.data(), sz);
  buf[sz] = 0;
  return buf;
}

}
