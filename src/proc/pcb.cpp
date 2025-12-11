#include "elf.h"
#include "pcb.h"
#include "schedule.h"
#include "../interrupt/sysret.h"
#include "../mem/kalloc.h"
#include "../utils/stl/unique_ptr.h"

extern int timer_tick;

namespace os {

static_storage<hashmap<int, pcb_t*>> pidmap;

int process_file_table::allocate(file *f, int fd) {
  f->ref();
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
      return i;
    }
  }
}

int process_file_table::allocate_from(file *f, int fd) {
  f->ref();

  // Find a usable descriptor from `fd`.
  for (int i = fd; ; i++) {
    if (!open.count(i)) {
      open[i] = f;
      desc[fd] = 0;
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
  for (auto [fd, f] : open)
    f->close();
  open.clear();
}

void pcb_t::clear() {
  pt::free(pt_root);
  pt_root = __kernel_pt_root;
  ftbl->clear();
  ftbl->drop();
  vfs->drop();
  clear_vma();
}

void pcb_t::clear_vma() {
  for (const auto &vma : this->vma) {
    if (vma.backup)
      vma.backup->drop();
  }
  vma.clear();
}

int pcb_t::open_file(const string &path, int flags, int mode) {
  return open_file_from(path, pwd, flags, mode);
}

int pcb_t::open_file_from(const string &path, int dirfd, int flags, int mode) {
  if (dirfd == AT_FDCWD)
    return open_file_from(path, pwd, flags, mode);
  
  if (!ftbl->count(dirfd))
    return -EBADF;

  auto entry = ftbl->at(dirfd)->entry;
  if (entry->node->type != inode::Dir)
    return -ENOTDIR;

  return open_file_from(path, entry, flags, mode);
}

int pcb_t::open_file_from(const string &path, dentry *relbase, int flags, int mode) {
  bool create = flags & O_CREAT;
  bool existok = !(flags & O_EXCL);
  bool write = (flags & 0x3) == O_RDWR || (flags & 0x3) == O_WRONLY;
  bool read = (flags & 0x3) == O_RDWR || (flags & 0x3) == O_RDONLY;

  // TODO: check search permission
  auto maybe_dentry = vfs->lookup_from(path, relbase);
  printk("opening: %s\n", path.c_str());
  if (!maybe_dentry) {
    if (!create)
      return maybe_dentry;

    auto parent = dirname(path);
    auto maybe_parent = vfs->lookup_from(parent, relbase);
    if (!maybe_parent)
      return maybe_parent;

    auto node = (*maybe_parent)->node;
    if (int err = node->create(basename(path), inode::File, mode); err != 0)
      return err;

    return open_file(path, flags & ~O_CREAT);
  }

  if (create && !existok)
    return -EEXIST;

  auto dentry = *maybe_dentry;
  inode *node = dentry->node;
  if (node->type == inode::Dir && write)
    return -EISDIR;

  if (read && !readable(euid, egid, node))
    return -EPERM;
  if (write && !writable(euid, egid, node))
    return -EPERM;

  file *f = new file(dentry, flags);
  int fd = ftbl->allocate(f);
  return fd;
}

int pcb_t::close_file(int fd) {
  if (!ftbl->count(fd))
    return -EINVAL;
  
  ftbl->deallocate(fd);
  return 0;
}

va_t pcb_t::brk(va_t addr) {
  auto va = (va_t) addr;
  auto lowest = stack_top;
  // Find the lowest VMA that we must not overlap with.
  // In other words, this is the cap of the address.
  for (auto &vma : this->vma) {
    if (vma.flags & VMA_IS_HEAP || vma.flags & VMA_IS_PT_LOAD)
      continue;
    lowest = min(lowest, vma.begin);
  }
  for (auto &vma : this->vma) {
    if (!(vma.flags & VMA_IS_HEAP))
      continue;

    if (va < vma.begin || va >= lowest)
      return vma.end;
    return vma.end = va;
  }
  panic("process has no heap!");
}

void tcb_t::send_signal(int sig) {
  if (mask[sig])
    return;
  pending.add(sig);
  if (status == Sleeping)
    scheduler.wakeup(this);
}

void pcb_t::send_signal(int sig) {
  // We find one eligible thread.
  for (auto x : threads) {
    if (x->mask[sig])
      continue;
    if (x->status == Sleeping) {
      x->pending.add(sig);
      scheduler.wakeup(x, /*can_preempt=*/ false);
      return;
    }
  }
  pending.add(sig);
  // TODO: When to retry delivery?
}

int tcb_t::sleep(size_t nano) {
  timeout = (nano + tick_length - 1) / tick_length;
  napping->push_back(this);
  auto ret = suspend();
  if (ret == -EINTR)
    return -EINTR;
  if (timeout != 0)
    return 1;
  return 0;
}

int nextpid() {
  static spinlock lock;
  static int pid = 0;
  synchronized syn(lock);
  return pid++;
}

void init_user(tcb_t *tcb) {
  va_t max = 0;
  auto pcb = tcb->pcb;
  for (const auto &vma : pcb->vma)
    max = os::max(vma.end, max);
  va_t heap_start = os::roundup<PAGE_SIZE>(max);

  // Allocate a heap. It is initially quite small.
  pcb->vma.push({
    .begin = heap_start, .end = heap_start + PAGE_SIZE, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE | VMA_IS_HEAP, .backup = nullptr, .offset = 0, .maxread = 0
  });
  // Allocate a stack. Note it grows downwards.
  pcb->vma.push({
    .begin = stack_top - user_stack_size, .end = tcb->usp = stack_top, .prot = PROT_READ | PROT_WRITE,
    .flags = MAP_PRIVATE | VMA_IS_STACK, .backup = nullptr, .offset = 0, .maxread = 0
  });
}

void init(tcb_t *tcb) {
  auto pcb = tcb->pcb;
  // Gives one physical page for the root page table and the kernel stack.
  pcb->pt_root = pframe();
  tcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb->pt_root), (void*) as_va(__kernel_pt_root), PAGE_SIZE);

  // Open stdin, stdout and stderr.
  // Note they are different files, but point to the same place.
  auto console = pcb->vfs->lookup("/dev/console");
  if (!console)
    panic("no console!");
  pcb->ftbl->allocate(new file(*console, O_RDONLY), 0); // stdin
  pcb->ftbl->allocate(new file(*console, O_WRONLY), 1); // stdout
  pcb->ftbl->allocate(new file(*console, O_WRONLY), 2); // stderr
}

void terminate(tcb_t *tcb, int ret) {
  // This will dispatch.
  scheduler.erase(tcb);
  auto pcb = tcb->pcb;
  pcb->clear();
  tcb->ret = ret;
  // Change all child processes to children of init.
  // It is expected that init will recycle them later.
  auto init = (*pidmap)[1];
  if (!init)
    panic("terminate: cannot find init");
  for (auto child : pcb->children) {
    child->parent = init;
    init->children.push_back(child);
  }
}

static void first_time_setup(tcb_t *tcb) {
  auto pcb = tcb->pcb;
  tcb->status = Running;
  // Construct a trap frame on the kernel stack.
  // Note that stack grows downwards, so we self-decrement
  // and leave the space for it.
  auto trap = (trapframe *) tcb->ksp;
  trap->sepc = tcb->pc;
  // Let sp point to the user stack.
  trap->sscratch = tcb->usp;

  int sstatus; CSRR(sstatus, sstatus);
  // User process with interrupt enabled.
  if (!pcb->kproc)
    sstatus = (sstatus & ~(1 << 8));
  else
    sstatus = (sstatus | (1 << 8));
  trap->sstatus = sstatus | (1 << 5);
  CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
}

void trap_return_setup(tcb_t *tcb) {
  [[unlikely]] if (tcb->status == Init) {
    first_time_setup(tcb);
    return;
  }

  tcb->status = Running;
  CSRW(satp, SATP_MODE_SV39 | (tcb->pcb->pt_root >> 12));
}

// Taken from <sched.h>

#define CLONE_VM      0x00000100 /* Set if VM shared between processes.  */
#define CLONE_FS      0x00000200 /* Set if fs info shared between processes.  */
#define CLONE_FILES   0x00000400 /* Set if open files shared between processes.  */
#define CLONE_SIGHAND 0x00000800 /* Set if signal handlers shared.  */
#define CLONE_PIDFD   0x00001000 /* Set if a pidfd should be placed in parent.  */
#define CLONE_PTRACE  0x00002000 /* Set if tracing continues on the child.  */
#define CLONE_VFORK   0x00004000 /* Set if the parent wants the child to wake it up on mm_release.  */
#define CLONE_PARENT  0x00008000 /* Set if we want to have the same parent as the cloner.  */
#define CLONE_THREAD  0x00010000 /* Set to add to same thread group.  */
#define CLONE_NEWNS   0x00020000 /* Set to create new namespace.  */
#define CLONE_SYSVSEM 0x00040000 /* Set to shared SVID SEM_UNDO semantics.  */
#define CLONE_SETTLS  0x00080000 /* Set TLS info.  */
#define CLONE_PARENT_SETTID 0x00100000 /* Store TID in userlevel buffer before MM copy.  */
#define CLONE_CHILD_CLEARTID 0x00200000 /* Register exit futex and memory location to clear.  */
#define CLONE_DETACHED 0x00400000 /* Create clone detached.  */
#define CLONE_UNTRACED 0x00800000 /* Set if the tracing process can't force CLONE_PTRACE on this clone.  */
#define CLONE_CHILD_SETTID 0x01000000 /* Store TID in userlevel buffer in the child.  */
#define CLONE_NEWCGROUP    0x02000000	/* New cgroup namespace.  */
#define CLONE_NEWUTS	0x04000000	/* New utsname group.  */
#define CLONE_NEWIPC	0x08000000	/* New ipcs.  */
#define CLONE_NEWUSER	0x10000000	/* New user namespace.  */
#define CLONE_NEWPID	0x20000000	/* New pid namespace.  */
#define CLONE_NEWNET	0x40000000	/* New network namespace.  */
#define CLONE_IO	0x80000000	/* Clone I/O context.  */
#define CLONE_NEWTIME	0x00000080  /* New time namespace */

int clone(unsigned flags, void *usp, void *tls) {
  // Parent thread/process.
  auto pt = active();
  auto pp = pt->pcb;

  // Child thread/process.
  auto ct = new tcb_t;
  pcb_t *cp;

  bool share_vm    = flags & CLONE_VM;
  bool share_files = flags & CLONE_FILES;
  bool share_fs    = flags & CLONE_FS;

  // When sharing virtual memory, we're essentially creating a thread.
  // We can reference to the same PCB.
  if (share_vm) {
    cp = pp;
  } else {
    // When not sharing, we're copying the PCB as well.
    cp = new pcb_t;
    cp->parent = pp;
    pp->children.push_back(cp);
    cp->pid = nextpid();
    (*pidmap)[cp->pid] = cp;

    // Mark the parent's table as copy-on-write.
    pt::walk((pte_t *) as_va(pp->pt_root), [](pte_t &pte) {
      // Only do this on user pages that are writable.
      if (!(pte & PTE_U) || !(pte & PTE_W))
        return;
      
      pte &= ~PTE_W;
      pte |= PTE_COW;
    });

    // Deep-copy the page table.
    cp->pt_root = pt::copy(pt_root());
    cp->vma = pp->vma;
  }

  ct->pcb = cp;
  cp->threads.push_back(ct);
  ct->tid = cp->nexttid();

  // Allocate a new kernel stack.
  ct->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  memcpy((char *) ct->ksp, (char *) pt->ksp, sizeof(trapframe));
  // Point to the new user stack.
  ct->usp = (va_t) usp;

  // Set the return value (a0) of child to zero.
  auto trap = (trapframe *) ct->ksp;
  trap->regs[8] = 0;
  ct->pc = trap->sepc;
  
  TLBRefreshGuard guard;

  // Copy the table, but not the files.
  cp->ftbl = share_files ? pp->ftbl : new process_file_table(*pp->ftbl);
  cp->ftbl->ref();
  for (auto [_, f] : *cp->ftbl)
    f->ref();

  // Copy VFS context.
  cp->vfs = share_fs ? pp->vfs : new vfs(*pp->vfs);
  cp->vfs->ref();

  // Copy various information from parent.
  ct->status = Ready;
  ct->tls = tls;

  // No need to copy them again. We have copied the entire structure.
  if (!share_vm) {
    cp->kproc = pp->kproc;
    cp->uid = pp->euid;
    cp->euid = pp->euid;
    cp->suid = pp->euid;
    cp->gid = pp->gid;
    cp->execpath = pp->execpath;
    cp->pwd = pp->pwd;
    cp->pgid = pp->pgid;
    cp->sid = pp->sid;
  }
  scheduler.add(ct);
  return cp->pid;
}

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
#define COPY_ENTRY(ty, val) \
    entry.type = ty; \
    entry.value = val; \
    copy_to_user(usp -= sizeof(auxv_entry), &entry, sizeof(auxv_entry));

int exec(const string &path, const vector<string> &argv, const vector<string> &envp) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  pt::free(pcb->pt_root);
  pcb->pt_root = __kernel_pt_root;
  // Reset the page table root immediately.
  {
    TLBRefreshGuard guard;
    CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
  }
  pcb->clear_vma();
  
  int fd = pcb->open_file(path, O_RDONLY);
  auto auxv = load_elf(pcb->ftbl->at(fd), tcb);
  if (!auxv) {
    printk("error: %d\n", auxv.error());
    // This process is in a bad state now. We must terminate it.
    // We cannot call clear() because that would double-free the root.
    //
    // Moreover, note that fd isn't opened so shouldn't be closed.
    pcb->ftbl->clear();
    pcb->ftbl->drop();
    pcb->clear_vma();
    pcb->vfs->drop();
    scheduler.erase(tcb);
    return auxv;
  }
  pcb->close_file(fd);
  pcb->execpath = path;
  pcb->execd = true;

  // Reallocate the page table and shallow-copy the higher half of kernel space.
  // We don't call init() because we don't change ksp, and don't reopen stdin/stdout/stderr.
  pcb->pt_root = pframe();
  memcpy((void *) as_va(pcb->pt_root), (void*) as_va(__kernel_pt_root), PAGE_SIZE);
  // Reset the page table root.
  {
    TLBRefreshGuard guard;
    CSRW(satp, SATP_MODE_SV39 | (pcb->pt_root >> 12));
  }

  tcb->status = Init;

  // Close files according to flags.
  os::vector<int> toclose;
  for (auto [fd, f] : *pcb->ftbl) {
    if (*pcb->ftbl->get_desc(fd) & FD_CLOEXEC
     || f->flags & O_CLOEXEC)
      toclose.push_back(fd);
  }
  for (auto fd : toclose)
    pcb->ftbl->deallocate(fd);

  char *usp = (char *) tcb->usp;
  os::vector<char*> argvp, envpp;
  // Copy the real contents of the strings.
  // Also copy the current path.
  copy_to_user(usp -= (path.size() + 1), path.c_str(), path.size() + 1);
  auto pathptr = usp;
  for (auto &str : envp) {
    int len = str.size() + 1;
    copy_to_user(usp -= len, str.c_str(), len);
    envpp.push_back(usp);
  }
  for (auto &str : argv) {
    int len = str.size() + 1;
    copy_to_user(usp -= len, str.c_str(), len);
    argvp.push_back(usp);
  }
  
  // Pad to 16-bytes.
  usp = rounddown<16>(usp);

  // If there is an even number of argv, envp and auxv combined together, then we'll
  // need an extra 8-byte padding here to counter for the argc.
  constexpr int AUXV_SIZE_DYNAMIC = 14;
  constexpr int AUXV_SIZE_STATIC = 8;
  const auto auxv_size = auxv->used ? AUXV_SIZE_DYNAMIC : AUXV_SIZE_STATIC;
  if ((argv.size() + envp.size() + auxv_size) % 2 == 0) {
    usp -= 8;
  }

  // Copy the AUXV entries.
  // TODO: consider ELF32 as well. busybox is ELF64 so doesn't matter.
  struct auxv_entry {
    long type;
    long value;
  } entry;
  
  // Refer to https://elixir.bootlin.com/musl/v1.2.2/source/ldso/dlstart.c,
  // as well as https://elixir.bootlin.com/musl/v1.2.2/source/ldso/dynlink.c.
  //
  // Note that CRT might need auxv entries as well, so we must include them
  // even if we don't use AUXV.
  
  // TODO: get real random source
  char *random;
  memcpy(random = usp -= 16, "aduhtkjm_1234567", 16);

  // Don't forget to change this when adding/removing entries!
  static_assert(AUXV_SIZE_DYNAMIC == 14);
  static_assert(AUXV_SIZE_STATIC == 8);
  COPY_ENTRY(AT_NULL, 0);
  if (auxv->used) {
    COPY_ENTRY(AT_EXECFN, (va_t) pathptr);
    COPY_ENTRY(AT_ENTRY, auxv->entry);
    COPY_ENTRY(AT_PHENT, sizeof(program_header));
    COPY_ENTRY(AT_PHDR, auxv->phdr);
    COPY_ENTRY(AT_PHNUM, auxv->phnum);
    COPY_ENTRY(AT_BASE, interp_pos);
  }
  COPY_ENTRY(AT_PAGESZ, PAGE_SIZE);
  COPY_ENTRY(AT_UID, pcb->uid);
  COPY_ENTRY(AT_GID, pcb->gid);
  COPY_ENTRY(AT_EUID, pcb->euid);
  COPY_ENTRY(AT_EGID, pcb->egid);
  COPY_ENTRY(AT_RANDOM, (va_t) random);
  COPY_ENTRY(AT_SECURE, 0);

  // Copy the pointers.
  // We copy envp pointers first, so that argv will be closer to stack top,
  // as required by the ABI.
  constexpr size_t ptrsz = sizeof(uintptr_t);

  // Insert a null pointer at the end of envp.
  uintptr_t nulptr = 0;
  copy_to_user(usp -= ptrsz, &nulptr, ptrsz);
  for (int i = int(envpp.size()) - 1; i >= 0; i--) {
    auto ptr = envpp[i];
    copy_to_user(usp -= ptrsz, &ptr, ptrsz);
  }

  // Insert a null pointer at the end of argv.
  copy_to_user(usp -= ptrsz, &nulptr, ptrsz);
  for (int i = int(argvp.size()) - 1; i >= 0; i--) {
    auto ptr = argvp[i];
    copy_to_user(usp -= ptrsz, &ptr, ptrsz);
  }

  size_t argc = argvp.size();
  copy_to_user(usp -= ptrsz, &argc, ptrsz);
  // We shouldn't maintain alignment; ld.so will do it for us.
  tcb->usp = (va_t) usp;
  assert(tcb->usp % 16 == 0);

  // Set up trapframe.
  auto trap = (trapframe *) tcb->ksp;
  trap->sepc = tcb->pc;
  trap->sscratch = tcb->usp;
  printk("exec done, pc = %p, usp = %p\n", tcb->pc, tcb->usp);
  trap->regs[2] = stack_top - user_stack_size; // TLS
  return 0;
}
#undef COPY_ENTRY

void copy_to_user(void *usr, const void *ker, size_t len) {
  EnableAccessToUserMemory enable;
  vma::map_current(usr, (char*) usr + len, /*write=*/true);
  memcpy(usr, ker, len);
}

expected<unique_ptr<char>> copy_from_user(void *usr, size_t len) {
  EnableAccessToUserMemory enable;
  vma::map_current(usr, (char *) usr + len);
  char *buf = new char[len];
  memcpy(buf, usr, len);
  return expected<unique_ptr<char>>(buf);
}

expected<unique_ptr<char>> copy_from_user(char *usr) {
  if (!usr)
    return expected<unique_ptr<char>>(nullptr);

  EnableAccessToUserMemory enable;
  vma::map_current(usr);
  vector<char> vec;
  char *p = usr;
  for (; p < roundup<PAGE_SIZE>(usr) && *p; p++) {
    vec.push_back(*p);
  }
  if (!*p)
    goto finish;

  for (int i = 0; i < 4096; i++) {
    vma::map_current(p);
    for (char *finish = p + PAGE_SIZE; p < finish && *p; p++)
      vec.push_back(*p);
    
    if (!*p)
      goto finish;
  }
  return -E2BIG;
finish:
  int sz = vec.size();
  char *buf = new char[1 + sz];
  memcpy(buf, vec.data(), sz);
  buf[sz] = 0;
  return expected<unique_ptr<char>>(buf);
}

expected<vector<string>> copy_from_user(char **usr) {
  if (!usr)
    return vector<string>();
  
  EnableAccessToUserMemory enable;
  vma::map_current(usr);
  vector<string> vec;
  char **p = usr;
  for (; p < roundup<PAGE_SIZE>(usr) && *p; p++) {
    auto str = copy_from_user(*p);
    if (!str)
      return str.error();
    vec.push_back(str->get());
  }
  if (!*p)
    goto finish;

  for (int i = 0; i < 4096; i++) {
    vma::map_current(p);
    for (char **finish = p + PAGE_SIZE; p < finish && *p; p++) {
      auto str = copy_from_user(*p);
      if (!str)
        return str.error();
      vec.push_back(str->get());
    }
    
    if (!*p)
      goto finish;
  }
  return -E2BIG;
finish:
  int sz = vec.size();
  char **buf = new char*[1 + sz];
  memcpy(buf, vec.data(), sz);
  buf[sz] = 0;
  return vec;
}

}
