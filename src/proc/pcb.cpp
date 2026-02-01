#include "elf.h"
#include "pcb.h"
#include "schedule.h"
#include "../interrupt/sysret.h"
#include "../mem/kalloc.h"
#include "../utils/stl/unique_ptr.h"
#include "../fs/pipe.h"
#include "../interrupt/impl.h"

extern int clock_period;

namespace os {

static_storage<hashmap<int, pcb_t*>> pidmap;
static_storage<hashmap<int, tcb_t*>> tidmap;

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
  for (int i = 0; ; i++) {
    if (!open.count(i)) {
      open[i] = f;
      desc[i] = 0;
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
      desc[i] = 0;
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
  for (auto [_, f] : open)
    f->close();
  open.clear();
  desc.clear();
}

process_file_table::process_file_table(const process_file_table &other): shared(), open(other.open), desc(other.desc) {
  // Increase pipe reader/writer count.
  for (auto [_, f] : open) {
    if (auto node = dyn_cast<pipe_inode>(f->node()))
      node->incf(f);
    f->ref();
  }
}

void pcb_t::clear() {
  setroot(pid, __kernel_pt_root);
  {
    pt::free(pt_root);
    pt_root = __kernel_pt_root;
  }
  ftbl->clear();
  ftbl->drop();
  vfs->drop();
  vma->drop();
  actor->drop();
}

int pcb_t::open_file(const string &path, int flags, int mode, inode::filetype type) {
  return open_file_from(path, pwd, flags, mode, type);
}

int pcb_t::open_file_from(const string &path, int dirfd, int flags, int mode, inode::filetype type) {
  if (path[0] == '\0')
    return -ENOENT;

  if (dirfd == AT_FDCWD || path[0] == '/')
    return open_file_from(path, pwd, flags, mode, type);

  if (!ftbl->count(dirfd))
    return -EBADF;

  auto entry = ftbl->at(dirfd)->entry;
  if (entry->node->type != inode::Dir)
    return -ENOTDIR;

  return open_file_from(path, entry, flags, mode, type);
}

int pcb_t::open_file_from(const string &path, dentry *relbase, int flags, int mode, inode::filetype type) {
  // Check maximum allowed files.
  size_t max = rlims[RLIMIT_OFILE].rlim_cur;
  if (max != 0 && ftbl->size() >= max)
    return -EMFILE;

  bool create = flags & O_CREAT;
  bool existok = !(flags & O_EXCL);
  bool write = (flags & 0x3) != O_RDONLY;
  bool read = (flags & 0x3) != O_WRONLY;
  bool follow = !(flags & O_NOFOLLOW);
  
  if (flags & O_PATH)
    write = read = false;

  auto maybe_dentry = vfs->lookup_from(path, relbase, follow);
  if (!maybe_dentry) {
    if (!create)
      return maybe_dentry;

    auto parent = dirname(path);
    auto maybe_parent = vfs->lookup_from(parent, relbase, follow);
    if (!maybe_parent)
      return maybe_parent;

    auto node = (*maybe_parent)->node;
    if (int err = node->create(basename(path), type, mode & ~umask); err != 0)
      return err;

    return open_file(path, flags & ~O_CREAT);
  }

  if (create && !existok)
    return -EEXIST;

  auto dentry = *maybe_dentry;
  inode *node = dentry->node;
  if (node->type == inode::Dir && write)
    return -EISDIR;
  if (node->type != inode::Dir && (flags & O_DIRECTORY))
    return -ENOTDIR;

  if (read && !readable(euid, egid, node))
    return -EACCES;
  if (write) {
    if (auto ret = writable(euid, egid, node); ret < 0)
      return ret;
  }

  file *f = new file(dentry, flags);
  if (flags & O_TRUNC)
    f->node()->truncate(0);
  
  int fd = ftbl->allocate(f);
  return fd;
}

expected<dentry*> pcb_t::obtain_file(const string &path, int dirfd, int flags) {
  if (path[0] == '\0')
    return -ENOENT;

  dentry *relbase = pwd;
  // Check relative path base.
  if (dirfd != AT_FDCWD && path[0] != '/') {
    if (!ftbl->count(dirfd))
      return -EBADF;

    auto entry = ftbl->at(dirfd)->entry;
    if (entry->node->type != inode::Dir)
      return -ENOTDIR;
    relbase = entry;
  }
  
  bool write = ((flags & 0x3) != O_RDONLY) & !(flags & O_PATH);
  bool read = ((flags & 0x3) != O_WRONLY) & !(flags & O_PATH);
  bool follow = !(flags & O_NOFOLLOW);

  auto maybe_dentry = vfs->lookup_from(path, relbase, follow);
  if (!maybe_dentry)
    return maybe_dentry;

  auto dentry = *maybe_dentry;
  inode *node = dentry->node;
  if (node->type == inode::Dir && write)
    return -EISDIR;
  if (node->type != inode::Dir && (flags & O_DIRECTORY))
    return -ENOTDIR;

  if (read && !readable(euid, egid, node))
    return -EACCES;
  if (write) {
    if (auto ret = writable(euid, egid, node); ret < 0)
      return ret;
  }

  return dentry;
}

expected<dentry*> pcb_t::obtain_file_emptyable(const string &name, int dirfd, int flags) {
  if (!(flags & O_EMPTYPATH))
    return obtain_file(name, dirfd, flags);
  
  if (dirfd == AT_FDCWD)
    return pwd;

  if (!ftbl->count(dirfd))
    return -ENOENT;

  return ftbl->at(dirfd)->entry;
}

int pcb_t::close_file(int fd) {
  if (!ftbl->count(fd))
    return -EBADF;
  
  ftbl->deallocate(fd);
  return 0;
}

void tcb_t::send_signal(int sig) {
  if (mask[sig] || status == Zombie)
    return;
  pending.add(sig);
  sigresume = sig;
  if (status == Sleeping)
    scheduler.wakeup(this);
}

void pcb_t::send_signal(int sig) {
  // We find one eligible thread.
  for (auto x : threads) {
    if (x->status == Zombie)
      continue;
    bool masked = x->mask[sig];
    bool waiting = x->sigresume == -2 && x->sigwait[sig];
    
    if (x->status == Sleeping && x->intr) {
      // Threads can't mask SIGKILL.
      // Moreover, masked signals won't affect sigtimedwait().
      if (waiting || !masked || sig == SIGKILL) {
        x->sigresume = sig;
        scheduler.wakeup(x, /*can_preempt=*/ false);
      }

      // If we're waking up a thread from sigtimedwait(), then this consumes the signal.
      if (waiting)
        return;
    }
    if (!masked) {
      x->pending.add(sig);
      return;
    }
  }
  pending.add(sig);
}

// We mainly fill in rlim[] defaults.
pcb_t::pcb_t() {
  rlims[RLIMIT_OFILE] = { .rlim_cur = 1024, .rlim_max = 4096 };
  rlims[RLIMIT_STACK] = { .rlim_cur = 8_mb, .rlim_max = 8_mb };
  rlims[RLIMIT_CORE]  = { .rlim_cur = 0,    .rlim_max = -1ul };
  rlims[RLIMIT_CPU]   = { .rlim_cur = -1ul, .rlim_max = -1ul };
  rlims[RLIMIT_RSS]   = { .rlim_cur = -1ul, .rlim_max = -1ul };
  rlims[RLIMIT_NPROC] = { .rlim_cur = 4096, .rlim_max = 8192 };
  rlims[RLIMIT_FSIZE] = { .rlim_cur = -1ul, .rlim_max = -1ul };
}
  

pcb_t::~pcb_t() {
  // Free all threads.
  for (auto it = threads.begin(); it != threads.end();) {
    auto next = it; ++next;
    delete *it;
    it = next;
  }
}

int tcb_t::sleep(size_t nano) {
  timeout = nano == -1ul ? (1l << 63) : 1 + (nano + tick_length - 1) / tick_length;
  
  wait_entry entry;
  napping.prepare(entry);
  
  int ret = 0;
  if (suspend() != 0)
    ret = -EINTR;
  else if (timeout != 0)
    ret = 1;

  napping.finish(entry);
  return ret;
}

int nextpid() {
  static spinlock lock;
  static int pid = 0;
  synchronized syn(lock);
  return pid++;
}

void terminate(tcb_t *tcb, int ret, bool sig) {
  auto pcb = tcb->pcb;

  assert(tcb->entr.size() == 0);
  tidmap->erase(tcb->tid);
  tcb->wake_robust_list();

  if (tcb->ctidaddr) {
    if (copy_to_user(tcb->ctidaddr, zeroes, sizeof(int)))
      detail::futex_wake(tcb->ctidaddr, 1);
  }

  if (pcb->threads.size() == 1) {
    assert(pcb->threads.front() == tcb);
    terminate(pcb, ret, sig);
    return;
  }

  // We don't remove it from `pcb->threads`, for rusage count and for reading return values.
  // Moreover, we need to track the threads so that we can recycle them on pcb exit.
  tcb->ret = ret;
  scheduler.erase(tcb);
}

// This should kill all threads inside this process.
void terminate(pcb_t *pcb, int ret, bool sig) {
  // Change all child processes to children of init.
  // It is expected that init will recycle them later.
  auto init = (*pidmap)[1];
  assert(init);
  for (auto child : pcb->children) {
    child->parent = init;
    init->children.push_back(child);
  }
  pcb->children.clear();
  pcb->ret = ret;

  pcb->clear();
  pidmap->erase(pcb->pid);
  pcb->sigterm = sig;

  // Remove existing itimer calls.
  itimer_real->erase(pcb->pid);

  // Erase all remaining threads.
  auto active = os::active();
  bool has_active = false; (void) has_active;
  for (auto t : pcb->threads) {
    t->wake_robust_list();
    if (t == active) {
      has_active = true;
      continue;
    }
    // It is possible that some other threads are sleeping, for example.
    for (auto [entry, queue] : t->entr)
      queue->finish(*entry);

    scheduler.erase(t);
  }
  assert(has_active);

  // Wake up parent for wait() system call.
  pcb->zombie = true;
  if (pcb->parent)
    pcb->parent->wait.wake_all(false);

  // Wake up process in CLONE_VFORK.
  pcb->vfork.wake_all(false);

  // Send a signal to parent.
  pcb->parent->send_signal(pcb->sigonterm);
  // Note: this does not return. It will dispatch a new thread.
  scheduler.erase(active);
}

#ifdef RV
static void first_time_setup(tcb_t *tcb) {
  auto pcb = tcb->pcb;
  tcb->status = Running;
  // Construct a trap frame on the kernel stack.
  // Note that stack grows downwards, so we self-decrement
  // and leave the space for it.
  auto trap = (trapframe *) tcb->ksp;

  int sstatus; CSRR(sstatus, sstatus);
  // User process with interrupt enabled.
  if (!pcb->kproc)
    sstatus = (sstatus & ~(1 << 8));
  else
    sstatus = (sstatus | (1 << 8));
  trap->sstatus = sstatus | (1 << 5);
  setroot(pcb->pid, pcb->pt_root);
}

void trap_return_setup(tcb_t *tcb) {
  [[unlikely]] if (tcb->status == Init) {
    first_time_setup(tcb);
    return;
  }

  tcb->status = Running;
  auto pcb = tcb->pcb;
  setroot(pcb->pid, pcb->pt_root);

  [[unlikely]] if (tcb->stidaddr) {
    bool succ = copy_to_user(tcb->stidaddr, &tcb->tid, sizeof(int)); (void) succ;
    assert(succ && "the memory should have been checked!");
    tcb->stidaddr = nullptr;
  }
}
#endif

#ifdef LA
static void first_time_setup(tcb_t *tcb) {
  auto pcb = tcb->pcb;
  tcb->status = Running;

  auto trap = (trapframe *) tcb->ksp;

  reg_t prmd = 0;
  prmd |= (3 << 0);   // PRMD.PPLV
  prmd |= (1 << 2);   // PRMD.PIE
  trap->prmd = prmd;
  trap->euen = 0;     // No FP yet.
}

void trap_return_setup(tcb_t *tcb) {
  [[unlikely]] if (tcb->status == Init) {
    first_time_setup(tcb);
    return;
  }

  tcb->status = Running;
}
#endif

tcb_t *clone(unsigned flags, va_t usp, void *tls, void *childtid) {
  // Parent thread/process.
  auto pt = active();
  auto pp = pt->pcb;

  // Child thread/process.
  auto ct = new tcb_t;
  pcb_t *cp;

  bool thread = flags & CLONE_THREAD;

  if (thread) {
    cp = pp;
    ct->tid = nextpid();
  } else {
    // When not sharing, we're copying the PCB as well.
    cp = new pcb_t;
    cp->parent = pp;
    pp->children.push_back(cp);
    cp->pid = nextpid();
    (*pidmap)[cp->pid] = cp;

    // Mark the parent's table as copy-on-write.
    TLBRefreshGuard guard;
    pt::walk((pte_t *) as_va(pp->pt_root), [](pte_t &pte) {
      // Only do this on user pages that are writable.
      if (!(pte & PTE_U) || !(pte & PTE_W) || (pte & PTE_SHARED))
        return;
      
      pte &= ~PTE_W;
      pte |= PTE_COW;
    });

    // Deep-copy the page table.
    cp->pt_root = pt::copy(pt_root());
    ct->tid = cp->pid;
  }
  (*tidmap)[ct->tid] = ct;

  cp->actor = flags & CLONE_SIGHAND ? pp->actor : new sigactor(*pp->actor);
  cp->actor->ref();
  
  cp->vma = flags & CLONE_VM ? pp->vma : new vma::addrspace(*pp->vma);
  cp->vma->ref();

  ct->pcb = cp;
  cp->threads.push_back(ct);

  // Allocate a new kernel stack.
  ct->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  memcpy((char *) ct->ksp, (char *) pt->ksp, sizeof(trapframe));
  // Point to the new user stack.
  // From man clone(2), when usp is NULL we reuse the parent stack.
  // Copy-on-write ensures this works.

  // Set the return value (a0) of child to zero.
  auto trap = (trapframe *) ct->ksp;
  trap->sscratch = usp ? usp : ((trapframe *) pt->ksp)->sscratch;
  trap->regs[8] = 0;

  // Copy the file table.
  cp->ftbl = flags & CLONE_FILES ? pp->ftbl : new process_file_table(*pp->ftbl);
  cp->ftbl->ref();

  // Copy VFS context.
  cp->vfs = flags & CLONE_FS ? pp->vfs : new vfs(*pp->vfs);
  cp->vfs->ref();

  ct->status = Ready;
  if (flags & CLONE_SETTLS)
    trap->regs[/*tp*/ 2] = (reg_t) tls;

  // Copy various information from parent, if we aren't sharing the PCB.
  if (!thread) {
    cp->kproc = pp->kproc;
    cp->uid = cp->euid = cp->suid = pp->uid;
    cp->gid = cp->egid = cp->sgid = pp->gid;
    cp->execpath = pp->execpath;
    cp->pwd = pp->pwd;
    cp->pgid = pp->pgid;
    cp->sid = pp->sid;
    memcpy(cp->rlims, pp->rlims, sizeof(pp->rlims));
  }

  if (flags & CLONE_CHILD_SETTID)
    ct->stidaddr = childtid;
  
  if (flags & CLONE_CHILD_CLEARTID)
    ct->ctidaddr = childtid;
  
  return ct;
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
#define COPY(usr, ker, len) \
  if (!copy_to_user(usr, ker, len)) \
    goto cleanup;

#define COPY_ENTRY(ty, val) \
  entry.type = ty; \
  entry.value = val; \
  COPY(usp -= sizeof(auxv_entry), &entry, sizeof(auxv_entry));

int exec(const string &path, const vector<string> &argv, const vector<string> &envp) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  assert(pcb->threads.size() == 1);

  // First check whether this is an ELF. If it isn't, we must not change anything.
  int fd = pcb->open_file(path, O_RDONLY);
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -ENOENT;
  if (!executable(pcb->euid, pcb->egid, file->node()))
    return -EACCES;

  auto oldvma = pcb->vma;
  pcb->vma = new vma::addrspace;
  goto proceed;
  
cleanup:
  delete pcb->vma;
  pcb->vma = oldvma;
  return -EFAULT;

proceed:
  // Try parse the shebang.
  char begin[2] {};
  file->read(&begin, 2);
  if (begin[0] == '#' && begin[1] == '!') {
    string interp; char v;
    for (int len = 1; len != 0; ) {
      len = file->read(&v, 1);
      if (len == 1) {
        if (v == '\n')
          break;
        interp.push_back(v);
      }
    }
    // We have to trim the string.
    unsigned x = 2;
    for (; x < interp.size() && interp[x] == ' '; x++);

    vector<string> newargv;
    if (auto y = interp.find(' ', x); y != string::npos) {
      newargv.reserve(argv.size() + 2);
      auto name = interp.substr(0, y);
      newargv.push_back(name);
      newargv.push_back(interp.substr(y + 1));
      interp = name;
    } else {
      newargv.reserve(argv.size() + 1);
      newargv.push_back(interp);
    }
    for (auto arg : argv)
      newargv.push_back(arg);

    return exec(interp, newargv, envp);
  }

  file->seek(0, file::begin);
  auto auxv = load_elf(file, tcb);
  if (!auxv) {
    // Do the rollback.
    delete pcb->vma;
    pcb->vma = oldvma;
    return auxv;
  }

  // Look at setuid bit.
  if (file->flags & 04000)
    pcb->euid = pcb->suid = file->node()->uid;  

  // Reset the page table root immediately. We're about to free the root.
  setroot(pcb->pid, __kernel_pt_root);
  {
    nopreempt _;
    pt::free(pcb->pt_root);
    pcb->pt_root = __kernel_pt_root;
  }
  // Note we must supply an absolute path.
  pcb->execpath = file->entry->path();
  pcb->close_file(fd);
  pcb->execd = true;

  // Reallocate the page table and shallow-copy the higher half of kernel space.
  // We don't call init() because we don't change ksp, and don't reopen stdin/stdout/stderr.
  pcb->pt_root = pframe();
  memcpy((void *) as_va(pcb->pt_root), (void*) as_va(__kernel_pt_root), PAGE_SIZE);
  // Reset the page table root.
  setroot(pcb->pid, pcb->pt_root);

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

  auto trap = (trapframe *) tcb->ksp;
  char *usp = (char *) trap->sscratch;
  os::vector<char*> argvp, envpp;
  // Copy the real contents of the strings.
  // Also copy the current path.
  COPY(usp -= (path.size() + 1), path.c_str(), path.size() + 1);
  auto pathptr = usp;
  for (auto &str : envp) {
    int len = str.size() + 1;
    COPY(usp -= len, str.c_str(), len);
    envpp.push_back(usp);
  }
  for (auto &str : argv) {
    int len = str.size() + 1;
    COPY(usp -= len, str.c_str(), len);
    argvp.push_back(usp);
  }
  
  // TODO: get real random source
  char *random;
  COPY(random = usp -= 16, "aduhtkjm_123456", 16);
  char *platform;
  COPY(platform = usp -= 8, "riscv64", 8);
  
  // Pad to 16-bytes.
  usp = rounddown<16>(usp);

  // If there is an even number of argv, envp and auxv combined together, then we'll
  // need an extra 8-byte padding here to counter for the argc.
  // Since each auxv entry is 16-byte, we don't need to worry for that.
  if ((argv.size() + envp.size()) % 2 == 0)
    usp -= 8;

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
  // even if we don't use ld.so.
  COPY_ENTRY(AT_NULL, 0);
  COPY_ENTRY(AT_BASE, auxv->interp ? interp_pos : 0);
  COPY_ENTRY(AT_ENTRY, auxv->entry);
  COPY_ENTRY(AT_PHENT, sizeof(program_header));
  COPY_ENTRY(AT_PHDR, auxv->phdr);

  COPY_ENTRY(AT_PHNUM, auxv->phnum);
  COPY_ENTRY(AT_EXECFN, (va_t) pathptr);
  COPY_ENTRY(AT_PAGESZ, PAGE_SIZE);
  COPY_ENTRY(AT_UID, pcb->uid);
  COPY_ENTRY(AT_GID, pcb->gid);

  COPY_ENTRY(AT_EUID, pcb->euid);
  COPY_ENTRY(AT_EGID, pcb->egid);
  COPY_ENTRY(AT_RANDOM, (va_t) random);
  COPY_ENTRY(AT_SECURE, 0);
  COPY_ENTRY(AT_SYSINFO_EHDR, 0);

  COPY_ENTRY(AT_CLKTCK, 1_s / tick_length); // This expects tick frequency.
  COPY_ENTRY(AT_PLATFORM, (va_t) platform);
  COPY_ENTRY(AT_HWCAP, 0);
  COPY_ENTRY(AT_HWCAP2, 0);
  COPY_ENTRY(AT_FLAGS, 0);

  // Copy the pointers.
  // We copy envp pointers first, so that argv will be closer to stack top,
  // as required by the ABI.
  constexpr size_t ptrsz = sizeof(uintptr_t);

  // Insert a null pointer at the end of envp.
  COPY(usp -= ptrsz, zeroes, ptrsz);
  for (int i = int(envpp.size()) - 1; i >= 0; i--) {
    auto ptr = envpp[i];
    COPY(usp -= ptrsz, &ptr, ptrsz);
  }

  // Insert a null pointer at the end of argv.
  COPY(usp -= ptrsz, zeroes, ptrsz);
  for (int i = int(argvp.size()) - 1; i >= 0; i--) {
    auto ptr = argvp[i];
    COPY(usp -= ptrsz, &ptr, ptrsz);
  }

  size_t argc = argvp.size();
  COPY(usp -= ptrsz, &argc, ptrsz);
  trap->sscratch = (va_t) usp;
  assert(trap->sscratch % 16 == 0);

  // Restore the signals.
  // Do note that `pcb->actor->sigact` is not of the same size as `struct sigactor`:
  // the latter has members inherited from `struct shared`.
  memset(pcb->actor->sigact, 0, sizeof(pcb->actor->sigact));

  // Now we can drop the old vma.
  pcb->vma->ref();
  oldvma->drop();
  return 0;
}
#undef COPY_ENTRY

bool copy_to_user(void *usr, const void *ker, size_t len) {
  EnableAccessToUserMemory enable;
  if (!vma::map_current(usr, (char*) usr + len, true))
    return false;
  memcpy(usr, ker, len);
  return true;
}

expected<unique_ptr<char>> copy_from_user(void *usr, size_t len) {
  EnableAccessToUserMemory enable;
  if (!vma::map_current(usr, (char *) usr + len, false))
    return false;
  char *buf = new char[len];
  memcpy(buf, usr, len);
  return expected<unique_ptr<char>>(buf);
}

bool copy_from_user(void *ker, void *usr, size_t len) {
  EnableAccessToUserMemory enable;
  if (!vma::map_current(usr, (char *) usr + len, false))
    return false;

  memcpy(ker, usr, len);
  return true;
}

expected<unique_ptr<char>> copy_from_user(char *usr) {
  if (!usr)
    return expected<unique_ptr<char>>(nullptr);

  EnableAccessToUserMemory enable;
  if (!vma::map_current(usr))
    return -EFAULT;
  vector<char> vec;
  char *p = usr;
  for (; p < roundup<PAGE_SIZE>(usr) && *p; p++)
    vec.push_back(*p);
  
  if ((va_t) p % PAGE_SIZE != 0 && !*p)
    goto finish;

  // As Linux does, we only support length <= 4096.
  if (!vma::map_current(p))
    return -EFAULT;
  for (char *finish = p + PAGE_SIZE; p < finish && *p; p++)
    vec.push_back(*p);
  
  if ((va_t) p % PAGE_SIZE != 0 && !*p)
    goto finish;
  
  return -ENAMETOOLONG;
finish:
  auto sz = vec.size();
  if (sz > 4096)
    return -ENAMETOOLONG;
  
  char *buf = new char[1 + sz];
  memcpy(buf, vec.data(), sz);
  buf[sz] = 0;
  return expected<unique_ptr<char>>(buf);
}

expected<vector<string>> copy_from_user(char **usr) {
  if (!usr)
    return vector<string>();
  
  EnableAccessToUserMemory enable;
  if (!vma::map_current(usr))
    return -EFAULT;

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
    if (!vma::map_current(p))
      return -EFAULT;

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
