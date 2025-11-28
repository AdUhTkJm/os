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

  class fs *fs = vfs->get(fsty, src);
  if (!fs)
    return -EINVAL;

  vfs->mount(mntpoint, fs->root);
  return 0;
}

// We must check the validity of user's pointer.
template<class T> requires (!is_pointer_v<T>)
optional<T*> asptr(reg_t t) {
  auto pcb = scheduler.active;
  va_t va = (va_t) t;
  for (const auto &vma : pcb->vma) {
    if (va < vma.end && va >= vma.begin)
      return (T*) va;
  }
  return nullopt;
}

optional<char**> aschptr(reg_t t) {
  auto pcb = scheduler.active;
  va_t va = (va_t) t;
  for (const auto &vma : pcb->vma) {
    if (va < vma.end && va >= vma.begin) {
      auto p = (char**) va;
      // We also check that each string in this char** is alright.
      for (char **q = p; *q; q++)
        if (!asptr<char>((reg_t) *q))
          return nullopt;
      return p;
    }
  }
  return nullopt;
}

/*
See table:
https://filippo.io/linux-syscall-table/
*/
long syscall(trapframe *ksp) {
  auto a0 = ksp->regs[8];
  auto a1 = ksp->regs[9];
  auto a2 = ksp->regs[10];
  auto a3 = ksp->regs[11];
  auto a7 = ksp->regs[15];
  auto pcb = scheduler.active;
  ksp->sepc += 4;
  switch (a7) {
  case 0: {
    // read(fd, buf, len)
    auto file = pcb->ftbl[a0];
    if (!file)
      return -ENOENT;

    auto maybe_buf = asptr<char>(a1);
    auto end = asptr<char>(a1 + a2);
    if (!maybe_buf || !end)
      return -EFAULT;
    char *buf = *maybe_buf;

    vma_map_current(buf, buf + a2);
    EnableAccessToUserMemory guard;
    auto len = file->read(buf, a2);
    return len;
  }
  case 1: {
    // write(fd, buf, len)
    auto file = pcb->ftbl[a0];
    if (!file)
      return -ENOENT;

    auto maybe_buf = asptr<char>(a1);
    auto end = asptr<char>(a1 + a2);
    if (!maybe_buf || !end)
      return -EFAULT;
    char *buf = *maybe_buf;

    vma_map_current(buf, (char*) buf + a2);
    EnableAccessToUserMemory guard;
    return file->write(buf, a2);
  }
  case 2: {
    // open(path, flags)
    auto path = asptr<char>(a0);
    if (!path)
      return -EFAULT;
    return pcb->open_file(*path, a1);
  }
  case 3: {
    // close(fd)
    return pcb->close_file(a0);
  }
  case 57: {
    // fork()
    return fork();
  }
  case 59: {
    // execve(path, argv, envp)
    EnableAccessToUserMemory guard;
    auto path = asptr<char>(a0);
    auto argv = aschptr(a1);
    auto envp = aschptr(a2);
    if (!path || !argv || !envp)
      return -EFAULT;
    return exec(*path, *argv, *envp);
  }
  case 60: {
    // exit(ret_code)
    os::terminate(pcb, a0);
    return 0;
  }
  case 165: {
    // mount(src, tgt, fsty, flags, data)
    // Ignore the data for now.
    EnableAccessToUserMemory guard;
    auto src = asptr<char>(a0);
    auto tgt = asptr<char>(a0);
    auto fsty = asptr<char>(a0);
    if (!src || !tgt || !fsty)
      return -EFAULT;
    return mount(*src, *tgt, *fsty, a3);
  }
  default:
    printk("unknown syscall: %d\n", a7);
    return -1;
  }
}

}

namespace os {

void interrupt_handler(reg_t scause, reg_t stval, void *sepc) {
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
      os::handle_plic_interrupt();
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
      trap->regs[8] = syscall(trap); // a0
      break;
    }
    case 12: // Instruction page fault
      vma_map_current((void*) stval);
      break;
    case 13: // Load page fault
      vma_map_current((void*) stval);
      break;
    case 15: // Store page fault
      vma_map_current((void*) stval);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %ld, sepc = %p\n", scause & 0xff, stval, sepc);
    }
  }
}

}
