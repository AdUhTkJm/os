#include "sysret.h"
#include "impl.h"
#include "../fs/tmpfs.h"
#include "../fs/vfs.h"
#include "../fs/devfs.h"
#include "../fs/net.h"
#include "../fs/tcp.h"
#include "../proc/schedule.h"
#include "../driver/tty/tty.h"
#include "../driver/virtio/virtio.h"
#include "../utils/log.h"
#include "../lock/futex.h"

namespace {

using namespace os;

int futex_wait(void *addr, int expected, void *_timeout, unsigned mask = -1) {
  size_t timeout = 1800'0000'0000'0000'0000ul;
  if (timeout) {
    timespec ts;
    if (!copy_from_user(&ts, (void *) _timeout, sizeof(timespec)))
      return -EFAULT;

    timeout = ts.tv_nsec + ts.tv_sec * 1_s;
  }

  int u;
  if (!copy_from_user(&u, addr, 4))
    return -EFAULT;

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

  if (!copy_from_user(&u, addr, 4))
    return -EFAULT;
  if (u != expected) {
    q->lock.release();
    return -EAGAIN;
  }

  futex_wait_entry entry;
  entry.mask = mask;
  auto tcb = active();
  for (;;) {
    q->wait.prepare(entry);
    q->lock.release();
    
    auto ret = tcb->sleep(timeout);

    q->lock.acquire();
    q->wait.finish(entry);
    
    if (!copy_from_user(&u, addr, 4)) {
      q->lock.release();
      return -EFAULT;
    }

    if (u != expected) {
      q->lock.release();
      return 0;
    }

    if (ret == -EINTR) {
      q->lock.release();
      return -EINTR;
    }

    // Timeout expired.
    if (ret == 0) {
      q->lock.release();
      return -ETIMEDOUT;
    }
  }
}

int futex_wake(void *addr, int count, unsigned mask = -1) {
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
  int woken = q->wait.wake(count, mask);
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
  case F_GETFL: {
    auto f = pcb->ftbl->at(fd);
    if (!f)
      return -EBADF;
    return f->flags;
  }
  case F_SETFL: {
    auto f = pcb->ftbl->at(fd);
    if (!f)
      return -EBADF;
    f->flags = arg;
    return 0;
  }
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

int mmap(unsigned long addr, unsigned long len, int prot, int flags, int fd, unsigned long offset) {
  auto pcb = active()->pcb;

  bool shared = flags & MAP_SHARED;
  bool priv = flags & MAP_PRIVATE;
  if ((!shared && !priv) || len == 0)
    return -EINVAL;

  bool fixed = flags & MAP_FIXED;
  bool anon = flags & MAP_ANONYMOUS;

  va_t start;
  if (!fixed) {
    start = pcb->vma.find_mmap(len, addr);
  } else
    start = rounddown<PAGE_SIZE>(addr);
  va_t end = roundup<PAGE_SIZE>(start + len);
  
  file *backup = nullptr;
  if (!anon && !pcb->ftbl->count(fd))
    return -EBADF;
  if (!anon) {
    backup = pcb->ftbl->at(fd);
    bool readable = (backup->flags & 3) != O_WRONLY;
    bool writable = (backup->flags & 3) != O_RDONLY;
    if (!readable || (shared && !writable))
      return -EACCES;
  } else if (shared)
    backup = new file(new dentry("<anon>", tmpfs->get(), nullptr), O_RDWR);
  
  if (shared) {
    pshared += end - start;
    backup->node()->cache = new page_cache(backup->node());
  }

  // Remove everything in start - end.
  munmap(start, len);

  // Now allocate near this cap. Note that this has to be page-aligned.
  vma::vma_t vma(start, end, prot, flags, backup, offset, len);
  pcb->vma.insert(vma);
  return vma.begin;
}

int mprotect(unsigned long start, unsigned long len, int prot) {
  if (len == 0)
    return -EINVAL;
  
  auto tcb = active();
  auto pcb = tcb->pcb;

  start = rounddown<PAGE_SIZE>(start);
  auto finish = roundup<PAGE_SIZE>(start + len);

  pcb->vma.split(start);
  pcb->vma.split(finish);

  auto overlap = pcb->vma.find_overlap(start, finish);
  if (overlap.size() == 0)
    return -ENOMEM;

  for (auto vma : overlap)
    vma->prot = prot;
  
  // Remap existing memory. TODO: change on page table instead!
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

int munmap(unsigned long start, unsigned long len) {
  // The initial check-and-split process is similar to mprotect.
  // Note we don't need memory contiguity here;
  // Also note the system call requires that `addr` is page-aligned.
  if (len == 0 || start % PAGE_SIZE != 0)
    return -EINVAL;

  auto tcb = active();
  auto pcb = tcb->pcb;

  auto finish = roundup<PAGE_SIZE>(start + len);
  
  pcb->vma.split(start);
  pcb->vma.split(finish);

  auto overlap = pcb->vma.find_overlap(start, finish);

  // The `overlap` vector is a list of pointers. They will be invalidated when we start to erase.
  vector<va_t> toremove;
  toremove.reserve(overlap.size());
  for (auto vma : overlap)
    toremove.push_back(vma->begin);

  if (overlap.size() == 0)
    return -ENOMEM;

  for (auto start : toremove)
    pcb->vma.erase(start);
  
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
    if (!copy_from_user(&dev.flags, argp, sizeof(termio)))
      return -EFAULT;
    return 0;
  }
  case TIOCGPGRP: {
    copy_to_user(argp, &dev.pgid, sizeof(int));
    return 0;
  }
  case TIOCSPGRP: {
    if (!copy_from_user(&dev.pgid, argp, sizeof(int)))
      return -EFAULT;
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
        pusage use {};
        for (auto t : child->threads)
          use += t->ruse;
        if (rusage) {
          struct rusage v = (struct rusage) use;
          copy_to_user(rusage, &v, sizeof(struct rusage));
        }

        pcb->cruse += use;
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
    // It is normal to receive a SIGCHLD, in which case we shouldn't return -EINTR.
    if (suspend() != 0 && tcb->sigresume != SIGCHLD) {
      pcb->wait.finish(entry);
      return -EINTR;
    }

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
  case SOCK_STREAM: {
    if (protocol != 0 && protocol != TCP)
      return -EINVAL;

    auto node = new tcp_socket_inode;
    auto f = new file(new dentry("", node, nullptr), O_RDWR | flags);
    return pcb->ftbl->allocate(f);
  }
  case SOCK_DGRAM: {
    if (protocol != 0 && protocol != UDP)
      return -EINVAL;

    auto node = new udp_socket_inode;
    auto f = new file(new dentry("", node, nullptr), O_RDWR | flags);
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

  // This is guaranteed to have enough size.
  sockaddr addr;
  if (len > sizeof(sockaddr))
    return -EINVAL;
  if (!copy_from_user(&addr, _addr, len))
    return -EFAULT;

  auto family = addr.sa_family;
  if (family != AF_INET) {
    printk("bind: unsupported family: %d\n", family);
    return -EINVAL;
  }

  if (auto udp = dyn_cast<udp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) &addr;
    return udp->bind(info.sin_addr.s_addr, info.sin_port);
  }
  
  if (auto tcp = dyn_cast<tcp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) &addr;
    return tcp->bind(info.sin_addr.s_addr, info.sin_port);
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

  if (len > sizeof(sockaddr))
    return -EINVAL;

  sockaddr addr;
  if (!copy_from_user(&addr, _addr, len))
    return -EFAULT;

  auto family = addr.sa_family;
  if (family != AF_INET) {
    printk("connect: unsupported family: %d\n", family);
    return -EINVAL;
  }

  if (auto udp = dyn_cast<udp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) &addr;
    return udp->connect(info.sin_addr.s_addr, info.sin_port);
  }
  if (auto tcp = dyn_cast<tcp_socket_inode>(file->node())) {
    if (len < sizeof(sockaddr_in))
      return -EINVAL;

    auto info = *(sockaddr_in *) &addr;
    return tcp->connect(info.sin_addr.s_addr, info.sin_port);
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

int futex(void *addr, int op, int val, void *timeout, unsigned long val2, unsigned long val3) {
  // We can ignore this - it's for optimization.
  if (op & FUTEX_PRIVATE_FLAG)
    op &= ~FUTEX_PRIVATE_FLAG;

  bool realtime = op & FUTEX_CLOCK_REALTIME;
  op &= ~FUTEX_CLOCK_REALTIME;

  if ((va_t) addr % 4 != 0)
    return -EINVAL;

  int u;
  if (!copy_from_user(&u, addr, 4))
    return -EFAULT;

  (void) val2;
  switch (op) {
  case FUTEX_WAIT:
    return futex_wait(addr, val, timeout);

  case FUTEX_WAKE:
    return futex_wake(addr, val);

  case FUTEX_WAIT_BITSET:
    if (val3 == 0)
      return -EINVAL;
    return futex_wait(addr, val, timeout, val3);

  case FUTEX_WAKE_BITSET:
    if (val3 == 0)
      return -EINVAL;
    return futex_wake(addr, val, val3);

  default:
    printk("unknown futex op: %d\n", op);
    return -EINVAL;
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

    int enable;
    if (!copy_from_user(&enable, optval, optlen))
      return -EFAULT;
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
  
  // Now let's see the inode type.
  if (dest) {
    // If it's a connection type, then it shouldn't connect.
    if (auto tcp = dyn_cast<tcp_socket_inode>(file->node()); tcp && tcp->get_state() != tcp::ESTABLISHED)
      return -ENOTCONN;

    if (auto ret = connect(fd, dest, addrlen); ret < 0)
      return ret;
  }

  auto node = file->node();
  int writeflags = 0;
  if (flags & MSG_DONTWAIT)
    writeflags |= O_NONBLOCK;

  char buf[1024];
  int written = 0;
  for (size_t i = 0; i < size; i += 1024) {
    size_t l = min(1024ul, size);
    if (!copy_from_user(buf, (char *) _buf + i, l))
      return -EFAULT;

    auto ret = node->write(0, buf, l, writeflags);
    if (ret <= 0)
      return written ? written : ret;
    size -= l;
    written += l;
  }
  return written;
}

int sendmsg(int fd, const msghdr &header, int flags) {
  if (header.msg_controllen != 0) {
    printk("sendmsg: no control message yet\n");
    return -EINVAL;
  }

  // Note msg_name and msg_namelen are user-space pointers, as expected by sendto().
  int sent = 0;
  for (unsigned i = 0; i < header.msg_iovlen; i++) {
    iovec iov;
    if (!copy_from_user(&iov, header.msg_iov + i, sizeof(iovec)))
      return -EFAULT;
    int ret = sendto(fd, iov.iov_base, iov.iov_len, flags, header.msg_name, header.msg_namelen);
    if (ret < 0)
      return sent;
    sent += ret;
  }
  return sent;
}

int sendmsg(int fd, void *msg, int flags) {
  msghdr header;
  if (!copy_from_user(&header, (void *) msg, sizeof(msghdr)))
    return -EFAULT;

  return sendmsg(fd, header, flags);
}

int prlimit64(int pid, int resource, void *newrlim, void *oldrlim) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  if (pid != 0 && pid != pcb->pid && pcb->uid != 0)
    return -EPERM; // TODO: better checks
  if (pid == 0)
    pid = pcb->pid;

  if (resource < 0 || (unsigned) resource >= sizeof(pcb->rlims) / sizeof(rlimit))
    return -EINVAL;
  
  auto before = pcb->rlims[resource];
  if (newrlim) {
    rlimit rlim;
    if (!copy_from_user(&rlim, newrlim, sizeof(rlimit)))
      return -EFAULT;

    if (before.rlim_max != 0 && (rlim.rlim_max > before.rlim_max || rlim.rlim_cur > rlim.rlim_max || rlim.rlim_cur == 0))
      return -EPERM;

    pcb->rlims[resource] = rlim;
  }

  if (newrlim) switch (resource) {
  case RLIMIT_STACK: {
    // TODO: actually reduce stack size
    break;
  }
  case RLIMIT_OFILE:
    // No special action needed.
    break;
  default:
    printk("prlimit: unknown resource = %d\n", resource);
    return -EINVAL;
  }

  if (oldrlim)
    copy_to_user(oldrlim, &before, sizeof(rlimit));
  return 0;
}

int nanosleep(int clock, int flags, void *rqtp, void *rmtp) {
  auto tcb = active();
  if (flags == 1) {
    printk("nanosleep: no abstime yet\n");
    return -EINVAL;
  }

  tcb->sclock = clock;
  timespec rq;
  if (!copy_from_user(&rq, rqtp, sizeof(timespec)))
    return -EFAULT;

  if (rq.tv_nsec >= (long) 1_s || rq.tv_sec < 0)
    return -EINVAL;

  size_t nano = rq.tv_sec * 1'000'000'000 + rq.tv_nsec;
  auto ret = tcb->sleep(nano);
  if (rmtp) {
    auto rem = tcb->timeout;
    timespec tm {
      .tv_sec = (long) (rem / 1_s),
      .tv_nsec = (long) (rem % 1_s),
    };
    copy_to_user(rmtp, &tm, sizeof(timespec));
    return -1;
  }
  return 0;
}

}
