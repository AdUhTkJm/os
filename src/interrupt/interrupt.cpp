#include "sysret.h"
#include "sysids.h"
#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../driver/plic/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"

// For a system call list:
// https://jborza.com/post/2021-05-11-riscv-linux-syscalls/

namespace {

using namespace os;

int mount(const char *src, const char *tgt, const char *fsty, unsigned long flags) {
  auto vfs = active()->pcb->vfs;
  auto maybe_mntpoint = vfs->lookup(tgt);
  if (!maybe_mntpoint)
    return -maybe_mntpoint;

  dentry *mntpoint = *maybe_mntpoint;
  if (mntpoint->node->type != inode::Dir)
    return -ENOTDIR;
  // This place is mounted.
  if (mntpoint->mnt)
    return -EBUSY;

  if (flags & MS_MOVE) {
    auto source = vfs->lookup(src);
    if (!source)
      return -ENOENT;
    vfs::move_mount(*source, mntpoint);
    return 0;
  }

  expected<class fs*> fs = vfs->get(fsty, src);
  if (!fs)
    return fs;

  vfs::mount(mntpoint, (*fs)->root);
  return 0;
}

// For details, see https://linux.die.net/man/2/fcntl
int fcntl(int fd, int ty, int arg) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  if (!pcb->ftbl.count(fd))
    return -EBADF;
  switch (ty) {
  case F_SETFD:
    pcb->ftbl.set_desc(fd, arg);
    return 0;
  case F_GETFD:
    return *pcb->ftbl.get_desc(fd);
  default:
    return -EINVAL;
  }
}

#define SYSHANDLE_BEGIN \
long syshandle(trapframe *ksp) { \
  auto a0 = ksp->regs[8];     \
  auto a1 = ksp->regs[9];     \
  auto a2 = ksp->regs[10];    \
  auto a3 = ksp->regs[11];    \
  auto a4 = ksp->regs[12];    \
  auto a7 = ksp->regs[15];    \
  auto tcb = active();        \
  auto pcb = tcb->pcb;        \
  ksp->sepc += 4;             \
  printk("syscall: %d\n", a7);\
  switch (a7) { {

#define SYSHANDLE_END \
  } default: \
    printk("unknown syscall: %d\n", a7); \
    return -1; \
  } \
}

#define ARGS0() 
#define ARGS1(a) reg_t a = a0;
#define ARGS2(a, b) reg_t a = a0, b = a1;
#define ARGS3(a, b, c) reg_t a = a0, b = a1, c = a2;
#define ARGS4(a, b, c, d) reg_t a = a0, b = a1, c = a2, d = a3;
#define ARGS5(a, b, c, d, e) reg_t a = a0, b = a1, c = a2, d = a3, e = a4;

#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1,_2,_3,_4,_5,_6,_7,_8,_9,N,...) N
#define PP_RSEQ_N() 9,8,7,6,5,4,3,2,1,0
#define DISPATCHER_IMPL(N) ARGS##N
#define DISPATCHER(N) DISPATCHER_IMPL(N)
#define ARGS(...) DISPATCHER(PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#define HANDLE(x, ...) } case syscall::x: { ARGS(__VA_ARGS__)

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

  file *f = pcb->ftbl[fd];
  if (!f)
    return -EBADF;

  if (f->node->type == inode::CharDevice)
    return -EINVAL;
  
  f->seek(offset, whence);
  return 0;
}

HANDLE(read, fd, _buf, len) {
  auto file = pcb->ftbl[fd];
  if (!file)
    return -ENOENT;

  char *buf = new char[len];
  auto ret = file->read(buf, len);
  copy_to_user((void *) _buf, buf, len);
  return ret;
}

HANDLE(write, fd, _buf, len) {
  auto file = pcb->ftbl[fd];
  if (!file)
    return -ENOENT;

  auto buf = copy_from_user((void*) _buf, len);
  if (!buf)
    return -EFAULT;
  auto ret = file->write(buf->get(), len);
  return ret;
}

HANDLE(openat, dirfd, _path, flags, mode) {
  (void) dirfd;
  // mode will be ignored when not creating file, as expected.
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  if ((*path)[0] != '/') { // Relative. Deal with it later.
    return -EBADF;
  }
  auto ret = pcb->open_file(path->get(), flags, mode);
  return ret;
}

HANDLE(close, fd) {
  return pcb->close_file(fd);
}

HANDLE(brk, addr) {
  return pcb->brk(addr);
}

HANDLE(dup, fd) {
  if (!pcb->ftbl.count(fd))
    return -EBADF;
  return pcb->ftbl.allocate(pcb->ftbl[fd]);
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

HANDLE(set_tid_address, _) {
  return tcb->tid; // TODO
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
  return fork();
}

HANDLE(execve, apath, aargv, aenvp) {
  EnableAccessToUserMemory guard;
  auto path = copy_from_user((char *) apath);
  auto argv = copy_from_user((char **) aargv);
  auto envp = copy_from_user((char **) aenvp);
  if (!path || !argv || !envp)
    return -EFAULT;
  return exec(path->get(), *argv, *envp);
}

HANDLE(exit, ret) {
  os::terminate(tcb, ret);
  return 0;
}

HANDLE(fcntl, fd, ty, args) {
  return fcntl(fd, ty, args);
}

HANDLE(getdents64, fd, dirents, cnt) {
  if (!pcb->ftbl.count(fd))
    return -EBADF;
  auto file = pcb->ftbl[a0];
  auto items = file->node->list();
  
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

HANDLE(mount, asrc, atgt, afsty, flags, data) {
  // Ignore the data for now.
  auto src = copy_from_user((char*) asrc);
  if (!src)
    return src;

  auto tgt = copy_from_user((char*) atgt);
  if (!tgt)
    return tgt;

  auto fsty = copy_from_user((char*) afsty);
  if (!fsty)
    return fsty;

  auto ret = mount(src->get(), tgt->get(), fsty->get(), flags);
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
      sbi_set_timer(rv_rdtime() + 3000000);
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
      vma_map_current((void*) stval);
      break;
    case 13: // Load page fault
      vma_map_current((void*) stval);
      break;
    case 15: // Store page fault. This also work on COW pages; no special care needed.
      vma_map_current((void*) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %p, sepc = %p\n", scause & 0xff, stval, sepc);
      os::terminate(active(), -127);
    }
  }
}

}
