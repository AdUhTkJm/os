#include "sysret.h"
#include "impl.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "../fs/net.h"
#include "../proc/schedule.h"
#include "../driver/tty/tty.h"
#include "../driver/virtio/virtio.h"
#include "../utils/log.h"
#include "../lock/futex.h"

namespace {

using namespace os;

int futex_wait(void *addr, int expected, void *timeout) {
  if (timeout)
    printk("futex_wait: no timeout now\n");
  
  auto p = copy_from_user(addr, 4);
  if (!p)
    return -EFAULT;

  int u = *(int *) p->get();
  if (u != expected)
    return -EAGAIN;

  futex_key key((va_t) addr);
  if (key.type == futex_key::BAD)
    return -EFAULT;

  futexes.lock.acquire();
  futex_queue *&q = (*futexes)[key];
  if (!q)
    q = new futex_queue;
  futexes.lock.release();

  q->lock.acquire();

  auto p2 = copy_from_user(addr, 4);
  u = *(int *) p2->get();
  if (u != expected) {
    q->lock.release();
    return -EAGAIN;
  }

  for (;;) {
    q->wait.wait(q->lock);
    
    auto p = copy_from_user(addr, 4);
    if (!p) {
      q->lock.release();
      return -EFAULT;
    }
    u = *(int *) p->get();
    if (u != expected)
      break;
  }

  q->lock.release();
  return 0;
}

int futex_wake(void *addr, int count) {
  futex_key key((va_t) addr);
  if (key.type == futex_key::BAD)
    return -EFAULT;

  futexes.lock.acquire();
  if (!futexes->count(key)) {
    futexes.lock.release();
    return -EINVAL;
  }

  futex_queue *q = (*futexes)[key];
  futexes.lock.release();

  q->lock.acquire();
  int woken = q->wait.notify(count);
  q->lock.release();
  return woken;
}

}

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

  [[unlikely]] if (pid == int(0x8000'0000))
    return -ESRCH;
  
  if (rusage)
    printk("wait4: unimplemented: rusage\n");
  if (!pcb->children.size())
    return -ECHILD;

  wait_entry entry;
  auto &lock = pcb->waitlock;
  lock.acquire();
  for (;;) {
    // Check whether a child has changed.
    // We must change first before we wait.
    for (auto child : pcb->children) {
      if (child->zombie) {
        int p = child->pid;
        // This is not the one we're looking for.
        if (p != pid && pid != -1)
          continue;

        lock.release();
        if (wstatus) {
          // See <wait.h> for the bits.
          int status = (child->ret & 0xff) << 8;
          copy_to_user(wstatus, &status, sizeof(int));
        }

        pcb->children.erase(child);
        delete child;
        return p;
      }
    }

    if (nohang) {
      lock.release();
      return 0;
    }

    pcb->wait.prepare(entry);
    lock.release();
    if (suspend() != 0)
      return -EINTR;
    lock.acquire();
    pcb->wait.finish(entry);
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

  int flags = 0;
  if (type & SOCK_CLOEXEC)
    flags |= O_NONBLOCK;
  if (type & SOCK_NONBLOCK)
    flags |= O_NONBLOCK;
  type &= ~SOCK_CLOEXEC;
  type &= ~SOCK_NONBLOCK;

  switch (type) {
  case SOCK_DGRAM: {
    if (protocol != 0 && protocol != UDP)
      return -EINVAL;

    auto node = new udp_socket_inode(virtio::netdev(), ip::src, 0);
    auto f = new file(new dentry("<sock>", node, nullptr), O_RDWR | flags);
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
    udp->bind(info.sin_addr.s_addr, info.sin_port);
    return 0;
  }

  printk("bind: not udp inode\n");
  return -EINVAL;
}

int connect(int fd, void *_addr, unsigned len) {
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
    printk("connect: unsupported family: %d\n", family);
    return -EINVAL;
  }

  // Now this might be TCP inode or UDP inode.
  // But we only have UDP now.
  if (auto udp = dyn_cast<udp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) addr;
    udp->connect(info.sin_addr.s_addr, info.sin_port);
    return 0;
  }

  printk("connect: not udp inode\n");
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

int futex(void *addr, int op, int val, void *timeout) {
  // We can ignore this - it's for optimization.
  if (op & FUTEX_PRIVATE_FLAG)
    op &= ~FUTEX_PRIVATE_FLAG;

  if ((va_t) addr % 4 != 0)
    return -EINVAL;
  auto p = copy_from_user(addr, 4);
  if (!p)
    return -EFAULT;
  int u = *(int *) p->get();

  switch (op) {
  case FUTEX_WAIT:
    return futex_wait(addr, val, timeout);

  case FUTEX_WAKE:
    return futex_wake(addr, val);

  default:
    printk("unknown futex op: %d\n", op);
  }
  return 0;
}

// See socket(7) for a list of options:
//   https://man7.org/linux/man-pages/man7/socket.7.html
int setsockopt(int fd, int level, int optname, void *optval, int optlen) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  if (level != 0)
    printk("setsockopt: unknown level: %d\n", level);

  switch (optname) {
  case SO_NO_CHECK: {
    // This has to be a UDP socket.
    if (optlen != 4)
      return -EINVAL;
    
    auto udp = dyn_cast<udp_socket_inode>(file->node());
    if (!udp)
      return -EBADF;

    auto p = copy_from_user(optval, optlen);
    if (!p)
      return p;
    int enable = *(int*) p->get();
    udp->options.checksum = !enable;
    return 0;
  }

  default:
    printk("setsockopt: unknown optname %d\n", level, optname);
    return -EINVAL;
  }
}

// From man send(2), we know the only difference between `send` and `write` is the presence of flags.
int sendto(int fd, void *_buf, unsigned long size, int flags, void *dest, unsigned int addrlen) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  auto buf = copy_from_user(_buf, size);
  if (!buf)
    return -EFAULT;
  
  // Now let's see the inode type.
  // TODO: if it's a connection type, then it shouldn't connect.
  if (dest) {
    if (auto ret = connect(fd, dest, addrlen); ret < 0)
      return ret;
  }

  auto node = file->node();
  int writeflags = 0;
  if (flags & MSG_DONTWAIT)
    writeflags |= O_NONBLOCK;

  return node->write(0, buf->get(), size, writeflags);
}

int sendmsg(int fd, const msghdr &header, int flags) {
  if (header.msg_controllen != 0) {
    printk("sendmsg: no control message yet\n");
    return -EINVAL;
  }

  auto iovp = copy_from_user(header.msg_iov, sizeof(iovec) * header.msg_iovlen);
  if (!iovp)
    return -EFAULT;

  iovec *iov = (iovec *) iovp->get();
  // printk("sendmsg: flags: %d\n", flags);

  // Note msg_name and msg_namelen are user-space pointers, as expected by sendto().
  int sent = 0;
  for (unsigned i = 0; i < header.msg_iovlen; i++) {
    int ret = sendto(fd, iov[i].iov_base, iov[i].iov_len, flags, header.msg_name, header.msg_namelen);
    if (ret < 0)
      return sent;
    sent += ret;
  }
  return sent;
}

int sendmsg(int fd, void *msg, int flags) {
  auto p = copy_from_user((void *) msg, sizeof(msghdr));
  if (!p)
    return -EFAULT;

  msghdr *header = (msghdr *) p->get();
  return sendmsg(fd, *header, flags);
}

int prlimit64(int pid, int resource, void *newrlim, void *oldrlim) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  if (pid != 0 && pid != pcb->pid && pcb->uid != 0)
    return -EPERM; // TODO: better checks
  if (pid == 0)
    pid = pcb->pid;

  switch (resource) {
  case RLIMIT_STACK: {
    auto before = pcb->rlims[RLIMIT_STACK];

    if (newrlim) {
      auto plim = copy_from_user(newrlim, sizeof(rlimit));
      if (!plim)
        return -EFAULT;

      auto rlim = *(rlimit *) plim->get();
      if (rlim.rlim_max > pcb->rlims[RLIMIT_STACK].rlim_max || rlim.rlim_cur > rlim.rlim_max)
        return -EPERM;

      pcb->rlims[RLIMIT_STACK] = rlim;
      // TODO: actually reduce stack size
    }

    if (oldrlim)
      copy_to_user(oldrlim, &before, sizeof(rlimit));
    return 0;
  }
  default:
    printk("prlimit: unknown resource = %d\n", resource);
    return -EINVAL;
  }
}

}
