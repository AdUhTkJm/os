#include "elf.h"
#include "pcb.h"
#include "schedule.h"
#include "../interrupt/sysret.h"
#include "../mem/kalloc.h"
#include "../utils/stl/unique_ptr.h"
#include "../fs/pipe.h"

extern int clock_period;

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
  for (auto [_, f] : open)
    f->close();
  open.clear();
  desc.clear();
}

process_file_table::process_file_table(const process_file_table &other): open(other.open), desc(other.desc) {
  // Increase pipe reader/writer count.
  for (auto [_, f] : open) {
    if (auto node = dyn_cast<pipe_inode>(f->node()))
      node->incf(f);
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
  vma.clear();
}

int pcb_t::open_file(const string &path, int flags, int mode, inode::filetype type) {
  return open_file_from(path, pwd, flags, mode, type);
}

int pcb_t::open_file_from(const string &path, int dirfd, int flags, int mode, inode::filetype type) {
  if (dirfd == AT_FDCWD)
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
    return -EPERM;

  bool create = flags & O_CREAT;
  bool existok = !(flags & O_EXCL);
  bool write = can_write(flags);
  bool read = can_read(flags);
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
    if (int err = node->create(basename(path), type, mode); err != 0)
      return err;

    return open_file(path, flags & ~O_CREAT);
  }

  if (create && !existok)
    return -EEXIST;

  auto dentry = *maybe_dentry;
  inode *node = dentry->node;
  if (node->type == inode::Dir && write)
    return -EISDIR;

  if (read && !readable(uid, gid, node))
    return -EPERM;
  if (write && !writable(uid, gid, node))
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
    if (x->mask[sig] || x->status == Zombie)
      continue;
    x->pending.add(sig);
    x->sigresume = sig;
    if (x->status == Sleeping)
      scheduler.wakeup(x, /*can_preempt=*/ false);
    return;
  }
  pending.add(sig);
}

pcb_t::~pcb_t() {
  // Free all threads.
  for (auto t = threads.front(); t;) {
    auto next = t->next;
    delete t;
    t = next;
  }
}

int tcb_t::sleep(size_t nano) {
  timeout = nano == -1ul ? (1l << 63) : (nano + tick_length - 1) / tick_length;
  
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
  auto pcb = tcb->pcb;

  assert(tcb->entr.size() == 0);

  if (pcb->threads.size() == 1) {
    assert(pcb->threads.front() == tcb);
    terminate(pcb, ret);
    return;
  }

  // We don't remove it from `pcb->threads`, for rusage count and for reading return values.
  // Moreover, we need to track the threads so that we can recycle them on pcb exit.
  tcb->ret = ret;
  scheduler.erase(tcb);
}

// This should kill all threads inside this process.
void terminate(pcb_t *pcb, int ret) {
  // Change all child processes to children of init.
  // It is expected that init will recycle them later.
  auto init = (*pidmap)[1];
  if (!init)
    panic("terminate: cannot find init");
  for (auto child : pcb->children) {
    child->parent = init;
    init->children.push_back(child);
  }
  pcb->children.clear();
  pcb->ret = ret;

  pcb->clear();
  pidmap->erase(pcb->pid);

  // Erase all remaining threads.
  auto active = os::active();
  bool has_active = false;
  for (auto t : pcb->threads) {
    if (t == active) {
      has_active = true;
      continue;
    }
    scheduler.erase(t);
  }

  // Wake up parent for wait() system call.
  pcb->zombie = true;
  if (pcb->parent)
    pcb->parent->wait.wake_all();

  // Send a signal to parent.
  pcb->parent->send_signal(SIGCHLD);
  if (has_active)
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
}
#endif

#ifdef LA
static void first_time_setup(tcb_t *tcb) {
  auto pcb = tcb->pcb;
  tcb->status = Running;

  auto trap = (trapframe *) tcb->ksp;

  trap->sepc = tcb->pc;
  trap->sscratch = tcb->usp;

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

tcb_t *clone(unsigned flags, va_t usp, void *tls) {
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
      if (!(pte & PTE_U) || !(pte & PTE_W))
        return;
      
      pte &= ~PTE_W;
      pte |= PTE_COW;
      pincref(PTE_TO_PA(pte));
    });

    // Deep-copy the page table.
    cp->pt_root = pt::copy(pt_root());
    cp->vma = pp->vma;
    ct->tid = cp->pid;
  }

  ct->pcb = cp;
  cp->threads.push_back(ct);

  // Allocate a new kernel stack.
  ct->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  memcpy((char *) ct->ksp, (char *) pt->ksp, sizeof(trapframe));
  // Point to the new user stack.
  // From man clone(2), when usp is NULL we reuse the parent stack.
  // Copy-on-write ensures this works.
  ct->usp = usp ? usp : pt->usp;

  // Set the return value (a0) of child to zero.
  auto trap = (trapframe *) ct->ksp;
  trap->regs[8] = 0;
  ct->pc = trap->sepc;

  // Copy the table, but not the files.
  cp->ftbl = share_files ? pp->ftbl : new process_file_table(*pp->ftbl);
  cp->ftbl->ref();
  for (auto [_, f] : *cp->ftbl)
    f->ref();

  // Copy VFS context.
  cp->vfs = share_fs ? pp->vfs : new vfs(*pp->vfs);
  cp->vfs->ref();

  ct->status = Ready;
  ct->tls = tls;

  // Copy various information from parent, if we aren't sharing the PCB.
  if (!share_vm) {
    cp->kproc = pp->kproc;
    cp->uid = cp->euid = cp->suid = pp->uid;
    cp->gid = cp->egid = cp->sgid = pp->gid;
    cp->execpath = pp->execpath;
    cp->pwd = pp->pwd;
    cp->pgid = pp->pgid;
    cp->sid = pp->sid;
    memcpy(cp->rlims, pp->rlims, sizeof(pp->rlims));
  }
  
  scheduler.add(ct);
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
#define COPY_ENTRY(ty, val) \
    entry.type = ty; \
    entry.value = val; \
    copy_to_user(usp -= sizeof(auxv_entry), &entry, sizeof(auxv_entry));

int exec(const string &path, const vector<string> &argv, const vector<string> &envp) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  assert(pcb->threads.size() == 1);

  // First check whether this is an ELF. If it isn't, we must not change anything.
  int fd = pcb->open_file(path, O_RDONLY);
  auto oldvma = pcb->vma;
  pcb->vma.clear();
  auto file = pcb->ftbl->at(fd);
  if (!file) {
    pcb->vma = oldvma;
    return -ENOENT;
  }

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
    pcb->vma = oldvma;
    return auxv;
  }

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
  
  // TODO: get real random source
  char *random;
  memcpy(random = usp -= 16, "aduhtkjm_123456", 16);
  char *platform;
  memcpy(platform = usp -= 8, "riscv64", 8);
  
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

bool copy_from_user(void *ker, void *usr, size_t len) {
  EnableAccessToUserMemory enable;
  vma::map_current(usr, (char *) usr + len);
  memcpy(ker, usr, len);
  return true;
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
