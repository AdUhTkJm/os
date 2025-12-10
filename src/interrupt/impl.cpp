#include "sysret.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "../proc/schedule.h"
#include "../driver/tty/tty.h"

namespace os::detail {

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
    auto source = vfs->lookup(src, /*lastsym=*/false);
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

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  switch (ty) {
  case F_SETFD:
    pcb->ftbl->set_desc(fd, arg);
    return 0;
  case F_GETFD:
    return *pcb->ftbl->get_desc(fd);
  case F_DUPFD:
    return pcb->ftbl->allocate_from(file, arg);
  case F_DUPFD_CLOEXEC: {
    int newfd = pcb->ftbl->allocate_from(file, arg);
    pcb->ftbl->set_desc(newfd, FD_CLOEXEC);
    return newfd;
  }
  default:
    return -EINVAL;
  }
}

int mprotect(unsigned long start, unsigned long len, int prot) {
  auto tcb = active();

  if (len == 0)
    return -EINVAL;
  start = rounddown<PAGE_SIZE>(start);
  auto finish = roundup<PAGE_SIZE>(start + len);
  auto pcb = tcb->pcb;
  if (!pcb->vma.has(start) || !pcb->vma.has(finish))
    return -ENOMEM;

  auto &vmas = pcb->vma;
  size_t begin = vmas.find(start);
  // Check memory contiguity.
  for (auto i = begin; i < vmas.size() && vmas[i].end < finish; i++) {
    if (vmas[i].end != vmas[i + 1].begin) 
      return -ENOMEM;
  }

  // Split the first VMA if needed at `start`. Now we're starting from the VMA to the right,
  // i.e. from `start` to original `vma.end`.
  if (vmas[begin].begin < start)
    vmas.split_at(begin++, start);

  // Note we don't include `finish` when we're mapping.
  size_t end = vmas.find(finish - 1);

  // Split the last VMA if needed at `end`. We don't need to update `end`, because this time
  // the split VMA is to the left.
  if (vmas[end].begin <= finish && finish < vmas[end].end)
    vmas.split_at(end, finish);

  // Now all VMAs that need changing are exactly those with indices in [begin, end].
  // Note this is inclusive on both ends.
  for (size_t i = begin; i <= end; ++i)
    vmas[i].prot = prot;
  
  // Remap existing memory.
  for (char *p = (char*) start; p != (char*) finish; p += PAGE_SIZE) {
    auto flags = pte_flags(p);
    if (flags == -1)
      continue;

    auto pa = to_pa(p);
    if (prot & PROT_EXEC) flags |= PTE_X;
    if (prot & PROT_READ) flags |= PTE_R;
    if (prot & PROT_WRITE) flags |= PTE_W;
    os::pmap(pa, (va_t) p, MAP_4KB, flags);
  }
  return 0;
}

int ioctl(int fd, int op, void *argp) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  auto tty = dyn_cast<tty_inode>(file->node());
  auto &dev = tty->tty;
  if (!tty)
    return -ENOTTY;

  switch (op) {
  case TCGETS: {
    termio io {
      .c_iflag = 0,
      .c_oflag = 0,
      .c_cflag = 0,
      .c_lflag = dev.flags,
      .c_line = 0,
      .c_cc = {}
    };
    copy_to_user(argp, &io, sizeof(io));
    return 0;
  }
  case TCSETS: {
    auto p = copy_from_user(argp, sizeof(termio));
    if (!p)
      return p;
    termio io = *(termio *) p->get();
    printk("flags: %x %x %x %x, char: %c\n", io.c_iflag, io.c_oflag, io.c_cflag, io.c_lflag, io.c_cc[0]);
    dev.flags = io.c_lflag;
    return 0;
  }
  case TIOCGPGRP: {
    copy_to_user(argp, &dev.pgid, sizeof(int));
    return 0;
  }
  case TIOCSPGRP: {
    auto p = copy_from_user(argp, sizeof(int));
    if (!p)
      return p;
    int pgid = *(int*) p->get();
    dev.pgid = pgid;
    return 0;
  }
  case TIOCGWINSZ: {
    winsize sz {
      .ws_row = dev.height,
      .ws_col = dev.width,
      .ws_xpixel = 800,
      .ws_ypixel = 600,
    };
    copy_to_user(argp, &sz, sizeof(sz));
    return 0;
  }
  default:
    return -EINVAL;
  }
}

}
