#include "sysret.h"
#include "sysids.h"
#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../driver/plic/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"

namespace {

using namespace os;

int mount(const char *src, const char *tgt, const char *fsty, unsigned long flags) {
  // Ignore flags for now.
  (void) flags;

  auto maybe_mntpoint = vfs->lookup(tgt);
  if (!maybe_mntpoint)
    return -maybe_mntpoint;

  dentry *mntpoint = *maybe_mntpoint;
  if (mntpoint->node->type != inode::Dir)
    return -ENOTDIR;
  if (vfs->mounted(mntpoint))
    return -EBUSY;

  expected<class fs*> fs = vfs->get(fsty, src);
  if (!fs)
    return fs;

  vfs->mount(mntpoint, (*fs)->root);
  return 0;
}

// For details, see https://linux.die.net/man/2/fcntl
int fcntl(int fd, int ty, int arg) {
  auto pcb = scheduler.active;
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

/*
See table:
https://jborza.com/post/2021-05-11-riscv-linux-syscalls/
*/
long syshandle(trapframe *ksp) {
  auto a0 = ksp->regs[8];
  auto a1 = ksp->regs[9];
  auto a2 = ksp->regs[10];
  auto a3 = ksp->regs[11];
  auto a7 = ksp->regs[15];
  auto pcb = scheduler.active;
  ksp->sepc += 4;
  switch (a7) {
  case lseek: { // lseek(fd, offset, whence)
    file::whence whence = 
      a3 == 0 ? file::begin
    : a3 == 1 ? file::current
    : a3 == 2 ? file::end
    : (file::whence) -1;
    if (int(whence) == -1)
      return -EINVAL;

    file *f = pcb->ftbl[a0];
    if (!f)
      return -EBADF;

    if (f->node->type == inode::CharDevice)
      return -EINVAL;
    
    f->seek(a1, whence);
    return 0;
  }
  case read: { // read(fd, buf, len)
    auto file = pcb->ftbl[a0];
    if (!file)
      return -ENOENT;

    char *buf = new char[a2];
    auto ret = file->read(buf, a2);
    copy_to_user((void *) a1, buf, a2);
    return ret;
  }
  case write: { // write(fd, buf, len)
    auto file = pcb->ftbl[a0];
    if (!file)
      return -ENOENT;

    auto buf = copy_from_user((void*) a1, a2);
    if (!buf)
      return -EFAULT;
    auto ret = file->write(buf->get(), a2);
    return ret;
  }
  case openat: {
    // open(dirfd, path, flags, mode)
    // mode will be ignored when not creating file, as expected.
    auto path = copy_from_user((char *) a1);
    if (!path)
      return -EFAULT;
    if ((*path)[0] != '/') { // Relative. Deal with it later.
      return -EBADF;
    }
    auto ret = pcb->open_file(path->get(), a2, a3);
    return ret;
  }
  case close: {
    // close(fd)
    return pcb->close_file(a0);
  }
  case brk: {
    // brk(addr)
    return pcb->brk(a0);
  }
  case dup: {
    // dup(fd)
    if (!pcb->ftbl.count(a0))
      return -EBADF;
    return pcb->ftbl.allocate(pcb->ftbl[a0]);
  }
  case getpid: {
    return pcb->pid;
  }
  case clone: {
    return fork();
  }
  case execve: {
    // execve(path, argv, envp)
    EnableAccessToUserMemory guard;
    auto path = copy_from_user((char *) a0);
    auto argv = copy_from_user((char **) a1);
    auto envp = copy_from_user((char **) a2);
    if (!path || !argv || !envp)
      return -EFAULT;
    return exec(path->get(), argv->get(), envp->get());
  }
  case exit: {
    // exit(ret_code)
    os::terminate(pcb, a0);
    return 0;
  }
  case syscall::fcntl: {
    // fcntl(fd, ty, args...)
    return fcntl(a0, a1, a2);
  }
  case getdents64: {
    // getdents(fd, dirents, cnt)
    if (!pcb->ftbl.count(a0))
      return -EBADF;
    auto file = pcb->ftbl[a0];
    auto items = file->node->list();
    
    char *pos = (char *) a1;
    for (const auto &item : items) {
      constexpr unsigned nameoff = offsetof(linux_dirent64, name);
      unsigned short len = nameoff + item.name.size() + 2;
      if (va_t(pos) - a1 + len >= va_t(a2))
        return -EINVAL;

      unsigned char type = inode::as_dt(item.ty);
      linux_dirent64 entry { .inum = (unsigned long) item.inum, ._resv = 0, .len = len, .type = type };
      copy_to_user(pos, &entry, nameoff);
      copy_to_user(pos + nameoff, item.name.c_str(), item.name.size() + 1);
      pos += len;
    }
    return va_t(pos) - a1;
  }
  case getppid: {
    // getppid()
    return pcb->parent->pid;
  }
  case syscall::mount: {
    // mount(src, tgt, fsty, flags, data)
    // Ignore the data for now.
    EnableAccessToUserMemory guard;
    auto src = copy_from_user((char*) a0);
    if (!src)
      return src;

    auto tgt = copy_from_user((char*) a1);
    if (!tgt)
      return tgt;

    auto fsty = copy_from_user((char*) a2);
    if (!fsty)
      return fsty;

    auto ret = mount(src->get(), tgt->get(), fsty->get(), a3);
    return ret;
  }
  default:
    printk("unknown syscall: %d\n", a7);
    return -1;
  }
}

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
    os::stackdump();
#endif
    panic("exception occurred in kernel");
  } else {
    switch (scause) {
    case 2: // Invalid instruction
      printk("exception (user): invalid instruction %p when executing %p\n", stval, sepc);
      os::terminate(scheduler.active, -127);
      break;
    case 8: { // System call
      auto pcb = scheduler.active;
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
    case 15: // Store page fault
      // This also work on COW pages. No special care needed.
      vma_map_current((void*) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
    }
  }
}

}
