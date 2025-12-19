#include "sysret.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "../fs/net.h"
#include "../proc/schedule.h"
#include "../driver/tty/tty.h"
#include "../driver/virtio/virtio.h"
#include "../utils/log.h"

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

int munmap(unsigned long addr, unsigned long len) {
  // The initial check-and-split process is similar to mprotect.
  // Note we don't need memory contiguity here;
  // Also note the system call requires that `addr` is page-aligned.
  auto tcb = active();
  if (len == 0 || addr % PAGE_SIZE != 0)
    return -EINVAL;

  auto finish = roundup<PAGE_SIZE>(addr + len);
  auto pcb = tcb->pcb;
  if (!pcb->vma.has(addr) || !pcb->vma.has(finish))
    return -ENOMEM;

  auto &vmas = pcb->vma;
  size_t begin = vmas.find(addr);
  if (vmas[begin].begin < addr)
    vmas.split_at(begin++, addr);

  size_t end = vmas.find(finish - 1);
  if (vmas[end].begin <= finish && finish < vmas[end].end)
    vmas.split_at(end, finish);

  // Now we do the real unmapping. In fact, we only need to remove everything in [begin, end].
  // Note both ends are inclusive.
  for (unsigned i = end; i < vmas.size(); i++)
    vmas[i - (end - begin)] = vmas[i];
  vmas.resize(vmas.size() - (end - begin));
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
    copy_to_user(argp, &dev.flags, sizeof(termio));
    return 0;
  }
  case TCSETS: {
    auto p = copy_from_user(argp, sizeof(termio));
    if (!p)
      return p;
    dev.flags = *(termio *) p->get();
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

int wait(int pid, void *wstatus, int options, void *rusage) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  bool nohang = options & WNOHANG;
  bool untraced = options & WUNTRACED;
  // TODO
  (void) untraced;
  spinlock lock;

  [[unlikely]] if (pid == int(0x8000'0000))
    return -ESRCH;
  
  if (pid != -1) {
    printk("wait4: unimplemented: pid = %d\n", pid);
    return -EINVAL;
  }
  if (rusage)
    printk("wait4: unimplemented: rusage\n");
  if (!pcb->children.size())
    return -ECHILD;

  for (;;) {
    // Check whether a child has changed.
    // We must change first before we wait.
    for (auto child : pcb->children) {
      if (child->zombie) {
        if (wstatus) {
          // See <wait.h> for the bits.
          int status = (child->ret & 0xff) << 8;
          copy_to_user(wstatus, &status, sizeof(int));
        }

        int pid = child->pid;
        pcb->children.erase(child);
        pcb->wait.erase(tcb);
        delete child;
        return pid;
      }
    }

    if (nohang)
      return 0;

    lock.acquire();
    pcb->wait.push_back(tcb);
    if (suspend(lock) != 0)
      return -EINTR;
  }
}

int faccessat(int dirfd, const char *path, int mode) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  bool relative = path[0] == '/';
  int flags = 0;
  int fd = relative
    ? pcb->open_file_from(path, dirfd, O_PATH)
    : pcb->open_file(path, O_PATH);

  if (fd < 0)
    return fd;

  if (mode == F_OK)
    return 0;

  auto node = pcb->ftbl->at(fd)->node();
  if (mode & R_OK && !(readable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  if (mode & W_OK && !(writable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  if (mode & X_OK && !(executable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  return 0;
}

int socket(int domain, int type, int protocol) {
  if (domain != AF_INET) {
    printk("socket: unsupported domain: %d\n", domain);
    return -EINVAL;
  }

  auto tcb = active();
  auto pcb = tcb->pcb;

  switch (type) {
  case SOCK_DGRAM: {
    if (protocol != 0 && protocol != UDP)
      return -EINVAL;

    auto node = new udp_socket_inode(virtio::netdev(), ip::src, 0);
    auto f = new file(new dentry("<sock>", node, nullptr), O_RDWR);
    return pcb->ftbl->allocate(f);
  }
  default:
    printk("socket: unsupported type: %d\n", type);
    return -EINVAL;
  }
}

int bind(int fd, void *_addr, unsigned len) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  auto addrp = copy_from_user(_addr, len);
  if (!addrp)
    return -addrp;
  auto addr = addrp->get();

  auto family = *(unsigned short *) addr;
  if (family != AF_INET) {
    printk("bind: unsupported family: %d\n", family);
    return -EINVAL;
  }

  // Now this might be TCP inode or UDP inode.
  // But we only have UDP now.
  if (auto udp = dyn_cast<udp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) addr;
    udp->src = info.sin_addr.s_addr;
    udp->srcport = info.sin_port;
    return 0;
  }

  printk("bind: not udp inode\n");
  return -EINVAL;
}

int syslog(int type, char *buf, unsigned long len) {
  constexpr size_t logsize = sizeof(log.buf);
  len = min(len, logsize);
  char kbuf[logsize];
  unsigned read = 0;
  switch (type) {
  case SYSLOG_ACTION_OPEN:
  case SYSLOG_ACTION_CLOSE:
    return 0;
  case SYSLOG_ACTION_READ: {
    read = log.read(kbuf, len);
    copy_to_user(buf, kbuf, read);
    return read;
  }
  case SYSLOG_ACTION_READ_ALL:
  case SYSLOG_ACTION_READ_CLEAR: {
    read = log.read_all(kbuf, len);
    copy_to_user(buf, kbuf, read);
    if (type == SYSLOG_ACTION_READ_ALL)
      return read;
    [[fallthrough]];
  }
  case SYSLOG_ACTION_CLEAR: {
    synchronized _(log.lock);
    log.head = log.tail = 0;
    return read;
  }
  case SYSLOG_ACTION_SIZE_UNREAD:
    return log.used();
  case SYSLOG_ACTION_SIZE_BUFFER:
    return logsize;
  default:
    printk("syslog: unknown functionality: %d\n", type);
    return -EINVAL;
  }
}

}
