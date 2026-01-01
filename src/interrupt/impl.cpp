#include "sysret.h"
#include "impl.h"
#include "../mem/shm.h"
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

namespace os::detail {

long futex_wait(void *addr, int expected, void *_timeout, unsigned mask) {
  size_t timeout = 1800'0000'0000'0000'0000ul;
  if (_timeout) {
    timespec ts;
    if (!copy_from_user(&ts, (void *) _timeout, sizeof(timespec)))
      return -EFAULT;

    if (ts.tv_nsec > 999'999'999 || ts.tv_nsec < 0)
      return -EINVAL;

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
  futex_queue *q = (*futexes)[key];
  if (!q) {
    q = new futex_queue;
    futexes->insert(key, q);
  }
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

long futex_wake(void *addr, int count, unsigned mask) {
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

long mount(const char *src, const char *tgt, const char *fsty, unsigned long flags) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto vfs = pcb->vfs;
  auto maybe_mntpoint = vfs->lookup_from(tgt, pcb->pwd);

  if (!maybe_mntpoint)
    return -maybe_mntpoint;

  dentry *mntpoint = *maybe_mntpoint;
  if (mntpoint->node->type != inode::Dir)
    return -ENOTDIR;
  // This place is mounted.
  if (mntpoint->mnt)
    return -EBUSY;

  if (flags & MS_MOVE) {
    auto source = vfs->lookup_from(src, pcb->pwd, /*lastsym=*/false);
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
long fcntl(int fd, int ty, int arg) {
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

long mmap(unsigned long addr, unsigned long len, int prot, int flags, int fd, unsigned long offset) {
  auto pcb = active()->pcb;

  bool shared = flags & MAP_SHARED;
  bool priv = flags & MAP_PRIVATE;
  if ((!shared && !priv) || len == 0)
    return -EINVAL;

  bool fixed = flags & MAP_FIXED;
  bool anon = flags & MAP_ANONYMOUS;

  va_t start;
  if (!fixed) {
    start = pcb->vma->find_mmap(len, addr);
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
  pcb->vma->insert(vma);
  return vma.begin;
}

long mprotect(unsigned long start, unsigned long len, int prot) {
  if (len == 0)
    return -EINVAL;
  
  auto tcb = active();
  auto pcb = tcb->pcb;

  start = rounddown<PAGE_SIZE>(start);
  auto finish = roundup<PAGE_SIZE>(start + len);

  pcb->vma->split(start);
  pcb->vma->split(finish);

  auto overlap = pcb->vma->find_overlap(start, finish);
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

long munmap(unsigned long start, unsigned long len) {
  // The initial check-and-split process is similar to mprotect.
  // Note we don't need memory contiguity here;
  // Also note the system call requires that `addr` is page-aligned.
  if (len == 0 || start % PAGE_SIZE != 0)
    return -EINVAL;

  auto tcb = active();
  auto pcb = tcb->pcb;

  auto finish = roundup<PAGE_SIZE>(start + len);
  
  pcb->vma->split(start);
  pcb->vma->split(finish);

  auto overlap = pcb->vma->find_overlap(start, finish);

  // The `overlap` vector is a list of pointers. They will be invalidated when we start to erase.
  vector<va_t> toremove;
  toremove.reserve(overlap.size());
  for (auto vma : overlap)
    toremove.push_back(vma->begin);

  if (overlap.size() == 0)
    return -ENOMEM;

  for (auto start : toremove)
    pcb->vma->erase(start);
  
  return 0;
}

long ioctl(int fd, int op, void *argp) {
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
    if (!copy_to_user(argp, &dev.flags, sizeof(termio)))
      return -EFAULT;
    return 0;
  }
  case TCSETS: {
    if (!copy_from_user(&dev.flags, argp, sizeof(termio)))
      return -EFAULT;
    return 0;
  }
  case TIOCGPGRP: {
    if (!copy_to_user(argp, &dev.pgid, sizeof(int)))
      return -EFAULT;
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
    if (!copy_to_user(argp, &sz, sizeof(sz)))
      return -EFAULT;
    return 0;
  }
  default:
    return -EINVAL;
  }
}

long wait(int pid, void *wstatus, int options, void *rusage) {
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
        if (p != pid && pid != -1 && !(pid < -1 && abs(pid) == child->pgid))
          continue;

        lock.release();
        if (wstatus) {
          // See <wait.h> for the bits.
          int status = child->sigterm ? (child->ret & 0x7f) : ((child->ret & 0xff) << 8);
          if (!copy_to_user(wstatus, &status, sizeof(int)))
            return -EFAULT;
        }
        pusage use {};
        for (auto t : child->threads)
          use += t->ruse;
        if (rusage) {
          struct rusage v = (struct rusage) use;
          if (!copy_to_user(rusage, &v, sizeof(struct rusage)))
            return -EFAULT;
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
    if (suspend() != 0 && tcb->sigresume != SIGCHLD && tcb->sigresume > 0) {
      pcb->wait.finish(entry);
      return -EINTR;
    }

    lock.acquire();
    pcb->wait.finish(entry);
  }
}

long faccessat(int dirfd, const char *path, int mode) {
  if (mode > (R_OK | W_OK | X_OK) || mode < 0)
    return -EINVAL;
  // We don't support empty paths, since we do not have `flags` in this call.
  if (path[0] == '\0')
    return -ENOENT;

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
  // According to man pages, faccessat(2) should use real ids rather than effective ids.
  if (mode & R_OK && !(readable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  if (mode & W_OK && !(writable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  if (mode & X_OK && !(executable(pcb->uid, pcb->gid, node)))
    return -EACCES;

  return 0;
}

long socket(int domain, int type, int protocol) {
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

long bind(int fd, void *_addr, unsigned len) {
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

long connect(int fd, void *_addr, unsigned len) {
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

long syslog(int type, char *buf, unsigned long len) {
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
    if (!copy_to_user(buf, kbuf, read))
      return -EFAULT;
    return read;
  }
  case SYSLOG_ACTION_READ_ALL:
  case SYSLOG_ACTION_READ_CLEAR: {
    read = log.read_all(kbuf, len);
    if (!copy_to_user(buf, kbuf, read))
      return -EFAULT;
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

long futex(void *addr, int op, int val, void *timeout, unsigned long val2, unsigned long val3) {
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
long setsockopt(int fd, int level, int optname, void *optval, int optlen) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  switch (optname) {
  case SO_NO_CHECK: {
    // This has to be a UDP socket.
    if (optlen != 4 || (level != 0 && level != UDP))
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
    printk("setsockopt: unknown optname %d at level %d\n", optname, level);
    return -ENOPROTOOPT;
  }
}

// From man send(2), we know the only difference between `send` and `write` is the presence of flags.
long sendto(int fd, void *_buf, unsigned long size, int flags, void *dest, unsigned int addrlen) {
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

  char buf[1460];
  size_t written = 0;
  while (written < size) {
    size_t l = min(1024ul, size);
    if (!copy_from_user(buf, (char *) _buf + written, l))
      return -EFAULT;

    auto ret = node->write(0, buf, l, writeflags);
    if (ret <= 0)
      return written ? written : ret;
    size -= l;
    written += l;
  }
  return written;
}

long sendmsg(int fd, const msghdr &header, int flags) {
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

long sendmsg(int fd, void *msg, int flags) {
  msghdr header;
  if (!copy_from_user(&header, (void *) msg, sizeof(msghdr)))
    return -EFAULT;

  return sendmsg(fd, header, flags);
}

long recvfrom(int fd, void *_buf, unsigned long size, int flags, void *src, unsigned int addrlen) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  auto node = file->node();
  int readflags = 0;
  if (flags & MSG_DONTWAIT)
    readflags |= O_NONBLOCK;


  // We read a single packet.
  char buf[1460];
  size_t l = min(1460ul, size);
  auto ret = node->read(0, buf, l, readflags);
  if (ret <= 0)
    return ret;

  if (!copy_to_user((char *) _buf, buf, l))
    return -EFAULT;
  size -= l;
  // We should copy incoming message's destination into `src`.
  if (src) {
    if (auto udp = dyn_cast<udp_socket_inode>(node)) {
      sockaddr_in result {
        .sin_family = AF_INET,
        .sin_port = udp->recvport,
        .sin_addr = { .s_addr = udp->recv },
      };
      if (!copy_to_user(src, &result, min(8u, addrlen)))
        return -EFAULT; 
    }
  }
  return ret;
}

long prlimit64(int pid, int resource, void *newrlim, void *oldrlim) {
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
    if (!copy_to_user(oldrlim, &before, sizeof(rlimit)))
      return -EFAULT;
  return 0;
}

long nanosleep(int clock, int flags, void *rqtp, void *rmtp) {
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
    if (!copy_to_user(rmtp, &tm, sizeof(timespec)))
      return -EFAULT;
    return -1;
  }
  return 0;
}

long clone(int flags, unsigned long stack, void *parenttid, void *tls, void *childtid) {
  auto tcb = active();

  // The signal on termination is for processes, rather than threads.
  if ((flags & 0xff) != 0 && (flags & CLONE_THREAD))
    return -EINVAL;

  // Disallowed: the same handler's user-space address might be different in different address spaces.
  // This also simplifies the case in VM.
  if (!(flags & CLONE_VM) && (flags & CLONE_SIGHAND))
    return -EINVAL;
  // Two threads must not share the same stack.
  if ((flags & CLONE_VM) & !stack)
    return -EINVAL;

  if (parenttid && (flags & CLONE_PARENT_SETTID)) {
    if (!copy_to_user((void *) parenttid, &tcb->tid, sizeof(int)))
      return -EFAULT;
  }

  tcb_t *ct = os::clone(flags, stack, (void *) tls, (void *) childtid);
  if (!(flags & CLONE_THREAD))
    ct->pcb->sigonterm = flags & 0xff;
  return ct->pcb->pid;
}

long shmget(int key, unsigned long len, int flags) {
  bool create = key == 0 || (flags & IPC_CREAT);
  bool excl = flags & IPC_EXCL;
  auto tcb = active();
  auto pcb = tcb->pcb;
  
  if (create) {
    if (key == 0)
      // Allocate a new key. We would expect most keys are unused.
      while (shm::shm->count(key = rand()));
    
    if (auto it = shm::shm->find(key); it != shm::shm->end()) {
      if (excl)
        return -EEXIST;
      auto [file, _] = (*it).second;
      file->close();
    }
    
    int perm = 0;
    if (flags & SHM_R)
      perm |= 0444;
    if (flags & SHM_W)
      perm |= 0222;

    auto node = new tmpfs_inode(&*tmpfs, pcb->uid, pcb->gid, perm, inode::File);
    node->truncate(len);
    auto *file = new class file(new dentry("", node, nullptr), O_RDWR);
    file->ref();
    struct shm::shared_memory::meta meta {
      .atime = 0, .dtime = 0, .ctime = now()
    };
    shm::shm->insert(key, { file, meta });
    return key;
  }

  // Retrieve the key.
  auto it = shm::shm->find(key);
  if (it == shm::shm->end())
    return -ENOENT;
  return key;
}

long shmat(int key, unsigned long addr, int flags) {
  if (addr && (flags & SHM_RND))
    addr = rounddown<PAGE_SIZE>(addr);
  else if (addr % PAGE_SIZE != 0)
    return -EINVAL;
  
  auto it = shm::shm->find(key);
  if (it == shm::shm->end())
    return -ENOENT;

  auto tcb = active();
  auto pcb = tcb->pcb;

  auto &[file, meta] = (*it).second;
  if (meta.removed)
    return -EIDRM;

  auto node = file->node();

  int prot = 0;
  if (node->mode & 4) {
    prot |= PROT_READ;
    if (flags & SHM_EXEC)
      prot |= PROT_EXEC;
  }
  if (node->mode & 2)
    prot |= PROT_WRITE;

  meta.atime = now();
  meta.attach++;

  auto fd = pcb->ftbl->allocate(file);
  return mmap(addr, node->size(), prot, MAP_SHARED, fd, 0);
}

long shmdt(unsigned long addr) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  // We find the exact starting point, rather than anything containing it.
  auto vma = pcb->vma->vmas.find(addr);
  if (!vma)
    return -EINVAL;
  auto backup = vma->backup;
  if (!backup)
    return -EINVAL;

  optional<int> key = nullopt;
  for (const auto &[k, value] : *shm::shm) {
    if (value.backup == backup) {
      key = k;
      break;
    }
  }
  if (!key)
    return -EINVAL;

  auto &[file, meta] = shm::shm->at(*key);
  meta.dtime = now();
  meta.attach--;
  pcb->vma->vmas.erase(vma->begin);
  return 0;
}

long shmctl(int key, int op, void *buf) {
  auto it = shm::shm->find(key);
  if (it == shm::shm->end())
    return -ENOENT;
  
  shmid_ds option;
  // Some operations don't need the option.
  if (op != IPC_RMID) {
    if (!copy_from_user(&option, buf, sizeof(shmid_ds)))
      return -EFAULT;
  }

  auto &[file, meta] = (*it).second;
  meta.ctime = now();
  switch (op) {
  case IPC_RMID:
    meta.removed = true;
    file->drop();
    return 0;
    
  default:
    printk("unknown op: %d\n", op);
    return -EINVAL;
  }
}

long getsockname(int fd, void *sockname, void *_len) {
  unsigned len;
  if (!copy_from_user(&len, _len, 4))
    return -EFAULT;

  auto tcb = active();
  auto pcb = tcb->pcb;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  auto node = file->node();
  if (auto udp = dyn_cast<udp_socket_inode>(node)) {
    sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = udp->srcport,
      .sin_addr = { .s_addr = udp->src },
    };
    int copy = min(len, (unsigned) sizeof(sockaddr_in));
    int needed = max(len, (unsigned) sizeof(sockaddr_in));
    if (!copy_to_user(sockname, &addr, copy))
      return -EFAULT;
    
    if (copy < needed) {
      if (!copy_to_user(_len, &needed, 4))
        return -EFAULT;
    }
  }

  if (auto tcp = dyn_cast<tcp_socket_inode>(node)) {
    sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = tcp->srcport,
      .sin_addr = { .s_addr = tcp->src },
    };
    int copy = min(len, (unsigned) sizeof(sockaddr_in));
    int needed = max(len, (unsigned) sizeof(sockaddr_in));
    if (!copy_to_user(sockname, &addr, copy))
      return -EFAULT;
    
    if (copy < needed) {
      if (!copy_to_user(_len, &needed, 4))
        return -EFAULT;
    }
  }

  return -ENOTSOCK;
}

long ppoll(void *_fds, unsigned int cnt, unsigned long timeout, void *sigmask, bool isuser) {
  // TODO: Ignore sigmask for now.
  (void) sigmask;
  if (cnt <= 0)
    return -EINVAL;

  // TODO: how to remove dynamic allocation here?
  unique_ptr<pollfd[]> fds;
  if (isuser) {
    fds.reset(new pollfd[cnt]);
    if (!copy_from_user(fds.get(), (void *) _fds, cnt * sizeof(pollfd)))
      return -EFAULT;
  } else {
    fds.reset((pollfd *) _fds);
  }

  auto tcb = active();
  auto pcb = tcb->pcb;
  
retry:
  int available = 0;
  for (long i = 0; i < cnt; i++) {
    pollfd &fd = fds[i];
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
    if (fd.revents != 0)
      available++;
  }
  if (available) {
    if (isuser) {
      if (!copy_to_user((void *) _fds, fds.get(), cnt * sizeof(pollfd)))
        return -EFAULT;
    }
    return available;
  }
  // Don't add to wait queue if we don't want to wait.
  if (!timeout)
    return 0;

  spinlock lock;
  lock.acquire();
  wait_entry *entries = new wait_entry[cnt * 2];
  for (long i = 0; i < cnt; i++) {
    pollfd fd = fds[i];
    auto file = pcb->ftbl->at(fd.fd);
    if (fd.events & POLLIN)
      file->node()->prepare_read_wait(entries[i * 2]);
    if (fd.events & POLLOUT)
      file->node()->prepare_write_wait(entries[i * 2 + 1]);
  }
  lock.release();

  // This calls suspend().
  auto ret = tcb->sleep(timeout);

  lock.acquire();
  for (long i = 0; i < cnt; i++) {
    pollfd fd = fds[i];
    auto file = pcb->ftbl->at(fd.fd);
    if (fd.events & POLLIN)
      file->node()->finish_read_wait(entries[i * 2]);
    if (fd.events & POLLOUT)
      file->node()->finish_write_wait(entries[i * 2 + 1]);
  }
  lock.release();
  
  delete[] entries;
  // Recovered from sleeping by timeout.
  if (ret == 0)
    return 0;
  // Recovered from sleeping by something other than signal.
  if (ret == 1)
    goto retry;
  // Recovered from sleeping by signal.
  return -EINTR;
}

long rename(int olddirfd, unsigned long oldpath, int newdirfd, unsigned long newpath, int flags) {
  auto pcb = active()->pcb;
  
  auto path = copy_from_user((char *) oldpath);
  if (!path || !*path)
    return -EFAULT;

  auto npath = copy_from_user((char *) newpath);
  if (!npath || !*npath)
    return -EFAULT;

  auto dir = dirname(path->get());
  int fd = pcb->open_file_from(dir, olddirfd, O_PATH);
  if (fd < 0)
    return fd;
    
  auto newdir = dirname(npath->get());
  int nfd = pcb->open_file_from(newdir, newdirfd, O_PATH);
  if (nfd < 0)
    return nfd;

  auto file = pcb->ftbl->at(fd);
  auto newfile = pcb->ftbl->at(nfd);
  auto node = file->node(), nnode = newfile->node();
  if (node->type != inode::Dir || nnode->type != inode::Dir)
    return -ENOTDIR;

  if (node->fs != nnode->fs)
    return -EXDEV;
  
  auto name = basename(path->get());
  vfs::invalidate(node, name);
  return node->move(name, nnode, basename(npath->get()), flags);
}

}
