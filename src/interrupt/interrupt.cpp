#include "sysret.h"
#include "sysids.h"
#include "impl.h"
#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../driver/plic/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"

// In nanosecond.
extern int timer_tick;

// For a system call list:
// https://jborza.com/post/2021-05-11-riscv-linux-syscalls/

namespace {

using namespace os;

#define SYSHANDLE_BEGIN \
long syshandle(trapframe *ksp) { \
  auto a0 = ksp->regs[8];     \
  auto a1 = ksp->regs[9];     \
  auto a2 = ksp->regs[10];    \
  auto a3 = ksp->regs[11];    \
  auto a4 = ksp->regs[12];    \
  auto a5 = ksp->regs[13];    \
  auto a7 = ksp->regs[15];    \
  auto tcb = active();        \
  auto pcb = tcb->pcb;        \
  ksp->sepc += 4;             \
  switch (a7) { {

#define SYSHANDLE_END \
  } default: \
    printk("unknown syscall: %d\n", a7); \
    return -ENOSYS; \
  } \
}

#define ARGS1(a) reg_t a = a0;
#define ARGS2(a, b) reg_t a = a0, b = a1;
#define ARGS3(a, b, c) reg_t a = a0, b = a1, c = a2;
#define ARGS4(a, b, c, d) reg_t a = a0, b = a1, c = a2, d = a3;
#define ARGS5(a, b, c, d, e) reg_t a = a0, b = a1, c = a2, d = a3, e = a4;
#define ARGS6(a, b, c, d, e, f) reg_t a = a0, b = a1, c = a2, d = a3, e = a4, f = a5;

#define PRINT_FORMAT(x) "syscall " #x " (%d): "
#define PRINT1(x, a) printk(PRINT_FORMAT(x) #a " = %p" "\n", syscall::x, a0);
#define PRINT2(x, a, b) printk(PRINT_FORMAT(x) #a " = %p, " #b " = %p" "\n", syscall::x, a0, a1);
#define PRINT3(x, a, b, c) printk(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p" "\n", syscall::x, a0, a1, a2);
#define PRINT4(x, a, b, c, d) printk(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p" "\n", syscall::x, a0, a1, a2, a3);
#define PRINT5(x, a, b, c, d, e) printk(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p" "\n", syscall::x, a0, a1, a2, a3, a4);
#define PRINT6(x, a, b, c, d, e, f) printk(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p, " #f " = %p" "\n", syscall::x, a0, a1, a2, a3, a4, a5);

#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, N, ...) N
#define PP_RSEQ_N() 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
#define DISPATCHER_IMPL(N) ARGS##N
#define DISPATCHER(N) DISPATCHER_IMPL(N)
#define DISPATCHER_PRINT_IMPL(N) PRINT##N
#define DISPATCHER_PRINT(N) DISPATCHER_PRINT_IMPL(N)
#define ARGS(...) DISPATCHER(PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#define PRINT(x, ...) DISPATCHER_PRINT(PP_NARG(__VA_ARGS__))(x, __VA_ARGS__)
#define HANDLE(x, ...) } case syscall::x: { ARGS(__VA_ARGS__) PRINT(x, __VA_ARGS__)

/*
See table:
https://jborza.com/post/2021-05-11-riscv-linux-syscalls/
*/
SYSHANDLE_BEGIN

HANDLE(lseek, fd, offset, _whence) {
  file::whence whence = 
    _whence == 0 ? file::begin
  : _whence == 1 ? file::current
  : _whence == 2 ? file::end
  : (file::whence) -1;
  if (int(whence) == -1)
    return -EINVAL;

  file *f = pcb->ftbl->at(fd);
  if (!f)
    return -EBADF;

  if (f->node()->type == inode::CharDevice)
    return -EINVAL;
  
  f->seek(offset, whence);
  return 0;
}

HANDLE(read, fd, _buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  char *buf = new char[len];
  auto ret = file->read(buf, len);
  copy_to_user((void *) _buf, buf, len);
  return ret;
}

HANDLE(write, fd, _buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -ENOENT;

  auto buf = copy_from_user((void*) _buf, len);
  if (!buf)
    return -EFAULT;
  auto ret = file->write(buf->get(), len);
  return ret;
}

HANDLE(writev, fd, iov, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -ENOENT;

  if (cnt <= 0)
    return -EINVAL;

  auto iovecs = copy_from_user((void*) iov, cnt * sizeof(iovec));
  if (!iovecs)
    return -EFAULT;

  auto iovk = (iovec *) iovecs->get();
  long total = 0;
  for (int i = 0; i < cnt; i++) {
    auto &v = iovk[i];

    if (v.iov_len == 0) continue;

    // Copy a single buffer.
    auto buf = copy_from_user(v.iov_base, v.iov_len);
    if (!buf)
      return total ? total : -EFAULT;

    ssize_t n = file->write(buf->get(), v.iov_len);
    if (n < 0)
      return total ? total : n;

    total += n;
    // If we have a partial write, then we stop immediately.
    if ((size_t) n < v.iov_len)
      break;
  }

  return total;
}

HANDLE(openat, dirfd, _path, flags, mode) {
  (void) dirfd;
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, O_RDONLY)
    : pcb->open_file(path->get(), O_RDONLY);
  return fd;
}

HANDLE(close, fd) {
  return pcb->close_file(fd);
}

HANDLE(readlinkat, dirfd, _path, buf, size) {
  (void) dirfd;
  if (size < 0)
    return -EINVAL;
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, O_RDONLY)
    : pcb->open_file(path->get(), O_RDONLY);
  if (fd < 0)
    return fd;
  auto f = pcb->ftbl->at(fd);
  auto link = f->node()->readlink();
  pcb->close_file(fd);
  if (!link)
    return -EINVAL;
  copy_to_user((void *) buf, link->c_str(), size);
  return min(link->size(), (unsigned long) size);
}

HANDLE(chdir, _path) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  auto fd = pcb->open_file(path->get(), O_RDONLY);
  if (fd < 0)
    return fd;
  pcb->pwd = pcb->ftbl->at(fd)->entry;
  return 0;
}

HANDLE(fchdir, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  pcb->pwd = file->entry;
  return 0;
}

HANDLE(brk, addr) {
  return pcb->brk(addr);
}

HANDLE(dup, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  return pcb->ftbl->allocate(file);
}

HANDLE(dup3, oldfd, newfd, flags) {
  if (oldfd == newfd)
    return -EINVAL;

  auto file = pcb->ftbl->at(oldfd);
  if (!file)
    return -EBADF;

  // Currently `flags` can only be these two values.
  if (flags && flags != O_CLOEXEC)
    return -EINVAL;

  pcb->ftbl->allocate(file, newfd);
  pcb->ftbl->set_desc(newfd, flags);
  return newfd;
}

HANDLE(getpid, _) {
  return pcb->pid;
}

HANDLE(getppid, _) {
  return pcb->parent->pid;
}

HANDLE(getuid, _) {
  return pcb->parent->uid;
}

HANDLE(geteuid, _) {
  return pcb->parent->euid;
}

HANDLE(getegid, _) {
  return pcb->parent->gid;
}

HANDLE(getgid, _) {
  return pcb->parent->egid;
}

HANDLE(gettid, _) {
  return tcb->tid;
}

HANDLE(set_tid_address, _) {
  return tcb->tid; // TODO
}

HANDLE(getrandom, _) {
  return 0; // TODO
}

HANDLE(setpgid, pid, pgid) {
  if (pid != pcb->pid)
    return -EPERM;
  auto ftgt = pidmap->find(pid);
  if (ftgt == pidmap->end())
    return -ESRCH;
  auto [_, tgt] = *ftgt;

  // Must be in the same session.
  if (pcb->sid != tgt->sid)
    return -EPERM;

  // Target must not be a session leader.
  if (tgt->sid == tgt->pid)
    return -EPERM;

  // Target cannot have executed `execve`.
  if (pcb->execd)
    return -EACCES;

  // Target must be owned by the process.
  if (pcb->uid != tgt->uid && pcb->euid != tgt->uid && pcb->uid != 0)
    return -EPERM;

  // Check the current session of pgid.
  for (const auto &[_, p] : *pidmap) {
    if (p->pgid == pgid) {
      if (p->sid != tgt->sid)
        return -EPERM;
      break;
    }
  }

  tgt->pgid = pgid;
  return 0;
}

HANDLE(getpgid, pid) {
  auto ftgt = pidmap->find(pid);
  if (ftgt == pidmap->end())
    return -ESRCH;
  auto [_, tgt] = *ftgt;

  if (pcb->uid != tgt->uid && pcb->euid != tgt->uid && pcb->uid != 0)
    return -EPERM;

  return tgt->pgid;
}

HANDLE(getcwd, buf, size) {
  auto path = pcb->pwd->path();
  if (path.size() + 1 >= (unsigned long) size)
    return -ERANGE;
  copy_to_user((void*) buf, path.c_str(), path.size() + 1);
  return 0;
}

HANDLE(uname, buf) {
  utsname name {
    .sysname = "Linux",
    .nodename = "",
    .release = "0.1",
    .version = "0.1",
    .machine = "RISC-V64",
  };
  
  return 0;
}

HANDLE(get_robust_list, pid, headptr, size) {
  auto queried = pid == 0 ? pcb : (*pidmap)[pid];
  // TODO: this is actually user memory.
  copy_to_user((void *) headptr, queried->robust_list, sizeof(robust_list_head));
  size_t v = sizeof(robust_list_head);
  copy_to_user((void *) size, &v, sizeof(size_t));
  return 0;
}

HANDLE(set_robust_list, headptr, size) {
  if (size != sizeof(robust_list_head))
    return -EINVAL;
  pcb->robust_list = (void*) headptr;
  return 0;
}

HANDLE(clone, flags, stack, parenttid, tls, childtid) {
  return os::clone(flags, (void *) stack, (void *) tls);
}

HANDLE(execve, _path, _argv, _envp) {
  EnableAccessToUserMemory guard;
  auto path = copy_from_user((char *) _path);
  auto argv = copy_from_user((char **) _argv);
  auto envp = copy_from_user((char **) _envp);
  if (!path || !argv || !envp)
    return -EFAULT;
  return exec(path->get(), *argv, *envp);
}

HANDLE(exit, ret) {
  os::terminate(tcb, ret);
  return 0; // Won't reach the thread.
}

HANDLE(exit_group, ret) {
  for (auto x : pcb->threads)
    os::terminate(x, ret);
  return 0; // Won't reach the thread.
}

HANDLE(fcntl, fd, ty, args) {
  return detail::fcntl(fd, ty, args);
}

HANDLE(getdents64, fd, dirents, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  auto items = file->node()->list();
  
  char *pos = (char *) dirents;
  for (const auto &item : items) {
    constexpr unsigned nameoff = offsetof(linux_dirent64, name);
    unsigned short len = nameoff + item.name.size() + 2;
    if (va_t(pos) - dirents + len >= va_t(cnt))
      return -EINVAL;

    unsigned char type = inode::as_dt(item.ty);
    linux_dirent64 entry { .inum = (unsigned long) item.inum, ._resv = 0, .len = len, .type = type };
    copy_to_user(pos, &entry, nameoff);
    copy_to_user(pos + nameoff, item.name.c_str(), item.name.size() + 1);
    pos += len;
  }
  return va_t(pos) - dirents;
}

HANDLE(mount, _src, _tgt, _fsty, flags, data) {
  // Ignore the data for now.
  auto src = copy_from_user((char*) _src);
  if (!src)
    return src;

  auto tgt = copy_from_user((char*) _tgt);
  if (!tgt)
    return tgt;

  auto fsty = copy_from_user((char*) _fsty);
  if (!fsty)
    return fsty;

  auto ret = detail::mount(src->get(), tgt->get(), fsty->get(), flags);
  (void) data;
  return ret;
}

HANDLE(chroot, apath) {
  auto path = copy_from_user((char *) apath);
  if (!path)
    return path;

  auto dentry = pcb->vfs->lookup(path->get(), /*lastsym=*/ false);
  if (!dentry)
    return dentry;
  auto mnt = (*dentry)->mnt;
  if (!mnt)
    return -EINVAL;
  return pcb->vfs->chroot(mnt);
}

HANDLE(prlimit64, pid, resource, new_rlim, old_rlim) {
  printk("pid = %d, pcb->pid = %d\n", pid, pcb->pid);
  if (pcb->pid != 0 && pid != pcb->pid)
    return -EPERM; // TODO: better checks

  printk("resource = %d\n", resource);
  // TODO
  return -1;
}

HANDLE(ioctl, fd, op, argp) {
  return detail::ioctl(fd, op, (void *) argp);
}

HANDLE(clock_gettime, id, tp) {
  timespec spec;
  if (id == CLOCK_MONOTONIC) {
    long time = rv_rdtime();
    spec.tv_sec = time / 1'000'000'000;
    spec.tv_nsec = time % 1'000'000'000;
    copy_to_user(&tp, &spec, sizeof(timespec));
    return 0;
  }
  return -1;
}

HANDLE(mmap, addr, len, prot, flags, fd, offset) {
  bool shared = flags & MAP_SHARED;
  bool priv = flags & MAP_PRIVATE;
  if ((!shared && !priv) || len == 0)
    return -EINVAL;

  bool fixed = flags & MAP_FIXED;
  bool anon = flags & MAP_ANONYMOUS;
  if (shared) {
    printk("no shared mmap yet\n");
    return -EINVAL;
  }

  va_t start, end;
  if (!fixed) {
    end = stack_top;
    // Find the lowest VMA that we must not overlap with.
    // In other words, this is the cap of the address.
    for (auto &vma : pcb->vma) {
      if (vma.flags & VMA_IS_HEAP || vma.flags & VMA_IS_PT_LOAD)
        continue;
      end = min(end, vma.begin);
    }
    end = rounddown<PAGE_SIZE>(end);
    start = rounddown<PAGE_SIZE>(end - len);
  } else {
    start = rounddown<PAGE_SIZE>(addr);
    end = roundup<PAGE_SIZE>(addr + len);
  }
  
  file *backup = nullptr;
  if (!anon && !pcb->ftbl->count(fd))
    return -EBADF;
  if (!anon) {
    backup = pcb->ftbl->at(fd);
    bool readable = backup->flags & 3 != O_WRONLY;
    bool writable = backup->flags & 3 != O_RDONLY;
    if (!readable || (shared && !writable))
      return -EACCES;
  }

  // Now allocate near this cap. Note that this has to be page-aligned.
  vma::vma_t vma {
    .begin = start, .end = end,
    .prot = (int) prot, .flags = (int) flags,
    .backup = backup, .offset = (size_t) offset,
    .maxread = (size_t) len
  };
  if (pcb->vma.push(vma) != result::success)
    return -EINVAL;
  return vma.begin;
}

HANDLE(mprotect, start, len, prot) {
  // Find the VMA that contains this mprotect.
  return detail::mprotect(start, len, prot);
}

HANDLE(munmap, addr, len) {
  return detail::munmap(addr, len);
}

HANDLE(rt_sigprocmask, how, set, oldset, size) {
  if (oldset)
    copy_to_user((void *) oldset, &tcb->mask.sig, size);
  if (!set)
    return 0;

  auto sigset = copy_from_user((void *) set, size);
  if (!sigset)
    return sigset.error();

  unsigned long mask;
  switch (size) {
  case 8:
    mask = *(unsigned char *) sigset->get();
    break;
  case 16:
    mask = *(unsigned short *) sigset->get();
    break;
  case 32:
    mask = *(unsigned *) sigset->get();
    break;
  default:
    return -EINVAL;
  };

  // Here `tcb->mask` means ignored signals, so the logic is reversed here.
  switch (how) {
  case SIG_BLOCK:
    tcb->mask.sig |= mask;
    break;
  case SIG_UNBLOCK:
    tcb->mask.sig &= ~mask;
    break;
  case SIG_SETMASK:
    tcb->mask.sig = mask;
    break;
  default:
    return -EINVAL;
  }
  return 0;
}

HANDLE(kill, pid, sig) {
  auto fproc = pidmap->find(pid);
  if (fproc == pidmap->end())
    return -ESRCH;
  auto [_, proc] = *fproc;
  
  if (proc->uid != pcb->uid && proc->uid != pcb->euid && pcb->uid != 0)
    return -EPERM;

  proc->send_signal(sig);
  return 0;
}

HANDLE(tgkill, pid, tid, sig) {
  auto fproc = pidmap->find(pid);
  if (fproc == pidmap->end())
    return -ESRCH;
  auto [_, proc] = *fproc;

  tcb_t *thread = nullptr;
  for (auto t : proc->threads) {
    if (t->tid == tid) {
      thread = t;
      break;
    }
  }
  if (!thread)
    return -EINVAL;

  // TODO: privilege check
  thread->send_signal(sig);
  return 0;
}

HANDLE(ppoll, _fds, cnt, tmo, sigmask) {
  // Ignore sigmask for now.
  if (cnt <= 0)
    return -EINVAL;
  auto pollfds = copy_from_user((void *) _fds, cnt * sizeof(pollfd));
  if (!pollfds)
    return pollfds;
retry:
  auto fds = (pollfd*) pollfds->get();
  int available = 0;
  for (long i = 0; i < cnt; i++) {
    pollfd fd = fds[i];
    if (fd.fd < 0) {
      fd.revents = POLLNVAL;
      continue;
    }

    auto file = pcb->ftbl->at(fd.fd);
    if (!file)
      return -EBADF;
    if (fd.events == 0)
      continue;

    fd.revents = file->node()->poll(fd.events);
    printk("given revents %d\n", fd.revents);
    if (fd.revents != 0)
      available++;
  }
  if (available) {
    copy_to_user((void *) _fds, fds, cnt * sizeof(pollfd));
    return available;
  }
  // Put to sleep with timeout. (How?)
  // TODO: must disable preempt here.
  for (long i = 0; i < cnt; i++) {
    pollfd fd = fds[i];
    auto file = pcb->ftbl->at(fd.fd);
    if (fd.events & POLLIN)
      file->node()->wait_on_read();
    if (fd.events & POLLOUT)
      file->node()->wait_on_write();
  }
  // What to do on interrupt?
  if (suspend() != 0)
    return 0;
  goto retry;
}

HANDLE(nanosleep, rqtp, rmtp) {
  auto m_rq = copy_from_user((void *) rqtp, sizeof(timespec));
  if (!m_rq)
    return m_rq;

  auto rq = *(timespec *) m_rq->get();
  if (rq.tv_nsec >= (long) 1_s || rq.tv_sec < 0)
    return -EINVAL;

  size_t nano = rq.tv_sec * 1'000'000'000 + rq.tv_nsec;
  printk("sleep: %ld ns\n", nano);
  size_t rem = tcb->sleep(nano);
  if (rmtp) {
    timespec tm {
      .tv_sec = (long) (rem / 1_s),
      .tv_nsec = (long) (rem % 1_s),
    };
    copy_to_user((void *) rmtp, &tm, sizeof(timespec));
    return -1;
  }
  return 0;
}

HANDLE(rt_sigaction, sig, act, oldact) {
  if (sig <= 0 || sig >= 32)
    return -EINVAL;

  if (oldact) {
    os::sigaction a = pcb->sigact[sig];
    ::sigaction v {
      .sa_handler = a.handler,
      .sa_flags = a.flags,
      .sa_mask = { a.mask.sig },
    };
    printk("old: handler = %p, mask = %p, flags = %p\n", a.handler, a.mask, a.flags);
    copy_to_user((void *) oldact, &v, sizeof(::sigaction));
  }
  if (act) {
    auto sigactp = copy_from_user((void *) act, sizeof(::sigaction));
    if (!sigactp)
      return sigactp;
    auto sigact = *(::sigaction *) sigactp->get();
    os::sigaction a {
      .handler = sigact.sa_handler,
      .mask = sigact.sa_mask.val,
      .flags = sigact.sa_flags
    };
    printk("new: handler = %p, mask = %p, flags = %p\n", a.handler, a.mask, a.flags);
  }
  return 0;
}

SYSHANDLE_END

}

namespace os {

[[gnu::no_instrument_function]] void interrupt_handler(reg_t scause, reg_t stval, void *sepc) {
  reg_t sstatus;
  CSRR(sstatus, sstatus);
  bool from_kernel = sstatus & (1 << 8);
  if (scause < 0) {
    int kind = scause & 0xff;
    switch (kind) {
    case 5: { // Timer interrupt
      // Tick every 100ms.
      sbi_set_timer(rv_rdtime() + tick_length / timer_tick);
      scheduler.tick();
      scheduler.yield(/*sleepy=*/false); // TODO: check time slice
    }
    case 9: // PLIC interrupt
      os::plic::handle();
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
#ifdef FUNC_INSTRUMENT
    os::stack::dump();
#endif
    panic("exception occurred in kernel");
  } else {
    switch (scause) {
    case 2: // Invalid instruction
      printk("exception (user): invalid instruction %p when executing %p\n", stval, sepc);
      os::terminate(active(), -127);
      break;
    case 5:
      printk("exception (user): load access fault at %p when executing %p\n", stval, sepc);
      printk("page table flags: %x, physical address: %p\n", pte_flags(stval), to_pa(stval));
      os::terminate(active(), -127);
      break;
    case 8: { // System call
      auto pcb = active();
      auto trap = (trapframe *) pcb->ksp;
      trap->regs[8] = syshandle(trap); // a0
      break;
    }
    case 12: // Instruction page fault
      vma::map_current((void*) stval);
      break;
    case 13: // Load page fault
      vma::map_current((void*) stval);
      break;
    case 15: // Store page fault. This also work on COW pages; no special care needed.
      vma::map_current((void*) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %p, sepc = %p\n", scause & 0xff, stval, sepc);
      os::terminate(active(), -127);
    }
  }
}

}
