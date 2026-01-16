#include "sysret.h"
#include "sysids.h"
#include "impl.h"
#include "../utils/libc.h"
#include "../utils/helper.h"
#include "../driver/plic/plic.h"
#include "../mem/kalloc.h"
#include "../proc/schedule.h"
#include "../fs/ext.h"
#include "../fs/net.h"
#include "../fs/tmpfs.h"
#include "../fs/pipe.h"
#include "../utils/log.h"

// In nanosecond.
extern int clock_period;
// In nanosecond, from Unix epoch. This is initially boot time.
extern size_t realtime;
// Default to zero (UTC).
timezone zone;

// Returns current timestamp.
size_t os::now() {
  return rdtime() * clock_period + realtime;
}

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

#ifndef NO_SYSCALL_LOG
const int IGNORED[] = { clock_gettime, getrusage, riscv_hwprobe };
static bool ignored(int x) {
  for (auto ignore : IGNORED) {
    if (x == ignore)
      return true;
  }
  return false;
}
#endif

#define PRINT_FORMAT(x) #x " (%d): "
#define LOG_METHOD printk
#define LOG(fmt, call, ...) (ignored(call) ? 0 : LOG_METHOD(fmt " (tid %d)", call, __VA_ARGS__, tcb->tid))
#define PRINT1(x, a) LOG(PRINT_FORMAT(x) #a " = %p", syscall::x, a0);
#define PRINT2(x, a, b) LOG(PRINT_FORMAT(x) #a " = %p, " #b " = %p", syscall::x, a0, a1);
#define PRINT3(x, a, b, c) LOG(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p", syscall::x, a0, a1, a2);
#define PRINT4(x, a, b, c, d) LOG(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p", syscall::x, a0, a1, a2, a3);
#define PRINT5(x, a, b, c, d, e) LOG(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p", syscall::x, a0, a1, a2, a3, a4);
#define PRINT6(x, a, b, c, d, e, f) LOG(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p, " #f " = %p", syscall::x, a0, a1, a2, a3, a4, a5);

#define PP_NARG(...) PP_NARG_(__VA_ARGS__, PP_RSEQ_N())
#define PP_NARG_(...) PP_ARG_N(__VA_ARGS__)
#define PP_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, N, ...) N
#define PP_RSEQ_N() 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
#define DISPATCHER_IMPL(N) ARGS##N
#define DISPATCHER(N) DISPATCHER_IMPL(N)
#define DISPATCHER_PRINT_IMPL(N) PRINT##N
#define DISPATCHER_PRINT(N) DISPATCHER_PRINT_IMPL(N)
#define ARGS(...) DISPATCHER(PP_NARG(__VA_ARGS__))(__VA_ARGS__)
#ifndef NO_SYSCALL_LOG
# define PRINT(x, ...) DISPATCHER_PRINT(PP_NARG(__VA_ARGS__))(x, __VA_ARGS__)
#else
# define PRINT(...)
#endif
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
  return f->offset;
}

HANDLE(read, fd, buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_WRONLY)
    return -EBADF;

  return detail::read_to_user(file, (void *) buf, len);
}

HANDLE(pread64, fd, buf, len, offset) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_WRONLY)
    return -EBADF;

  SeekGuard _(file, offset);
  return detail::read_to_user(file, (void *) buf, len);
}

HANDLE(readv, fd, iov, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_WRONLY)
    return -EBADF;

  if (cnt <= 0)
    return -EINVAL;

  long total = 0;
  for (int i = 0; i < cnt; i++) {
    iovec v;
    if (!copy_from_user(&v, (char *) iov + i * sizeof(iovec), sizeof(iovec)))
      return -EFAULT;

    if (v.iov_len == 0)
      continue;

    // Copy a single buffer.
    ssize_t n = detail::read_to_user(file, (void *) v.iov_base, v.iov_len);
    if (n <= 0)
      return total ? total : n;

    total += n;
    // If we have a partial read, then we stop immediately.
    if ((size_t) n < v.iov_len)
      break;
  }

  return total;
}

HANDLE(write, fd, buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_RDONLY)
    return -EBADF;

  return detail::write_from_user(file, (void *) buf, len);
}

HANDLE(pwrite64, fd, buf, len, offset) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_RDONLY)
    return -EBADF;

  SeekGuard _(file, offset);
  return detail::write_from_user(file, (void *) buf, len);
}

HANDLE(writev, fd, iov, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 3) == O_RDONLY)
    return -EBADF;

  if (cnt <= 0)
    return -EINVAL;

  long total = 0;
  char buf[1024];
  for (int i = 0; i < cnt; i++) {
    iovec v;
    if (!copy_from_user(&v, (iovec *) iov + i, sizeof(iovec)))
      return -EFAULT;

    if (v.iov_len == 0)
      continue;

    // Copy a single buffer.
    size_t n = detail::write_from_user(file, (void *) v.iov_base, v.iov_len);
    if (n <= 0)
      return total ? total : n;

    total += n;
    // If we have a partial write, then we stop immediately.
    if (n < v.iov_len)
      break;
  }

  return total;
}

HANDLE(truncate, _path, len) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  auto fd = pcb->obtain_file(path->get(), AT_FDCWD, O_WRONLY);
  if (!fd)
    return fd;

  return (*fd)->node->truncate(len);
}

HANDLE(ftruncate, fd, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  return file->node()->truncate(len);
}

HANDLE(fallocate, fd, mode, offset, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file || (file->flags & 0x3) == O_RDONLY)
    return -EBADF;

  if (offset < 0 || len <= 0)
    return -EINVAL;

  // TODO: do real allocation. We don't really have holes yet, so let's just truncate.
  auto node = file->node();
  auto newsize = (unsigned long) offset + len;
  if (newsize >= 4_gb)
    return -EFBIG;

  if (newsize <= node->size())
    return 0;

  if (mode != 0) {
    printk("fallocate: unknown mode: %d\n", mode);
    return -EINVAL;
  }

  return node->truncate(newsize);
}

HANDLE(mkdirat, dirfd, _path, mode) {
  auto pathp = copy_from_user((char *) _path);
  if (!pathp)
    return pathp;
  if (!*pathp)
    return -EFAULT;

  const char *path = pathp->get();
  int flags = O_PATH | O_CREAT | O_EXCL;
  int fd = pcb->open_file_from(path, dirfd, flags, mode & ~pcb->umask, inode::Dir);
  return fd < 0 ? fd : 0;
}

HANDLE(mknodat, dirfd, _path, mode, dev) {
  auto pathp = copy_from_user((char *) _path);
  if (!pathp)
    return -pathp;
  if (!*pathp)
    return -EINVAL;

  const char *path = pathp->get();
  if (pcb->vfs->lookup(path))
    return -EEXIST;

  auto dir = dirname(path);
  auto fd = pcb->obtain_file(dir, dirfd, O_RDONLY);
  if (!fd)
    return fd;
  
  auto node = (*fd)->node;
  auto inodety = ext_inode::totype((ext_inode::ftypeflags) (mode & ~0777));
  if (inodety == inode::Bad)
    return -EINVAL;
  if (inodety == inode::BlockDevice || inodety == inode::CharDevice) {
    printk("mknodat: unsupported: dev: %d\n", dev);
    return -EINVAL;
  }
  return node->create(basename(path), inodety, mode & ~pcb->umask);
}

HANDLE(sendfile, out, in, offptr, len) {
  auto fout = pcb->ftbl->at(out);
  if (!fout || !can_write(fout->flags))
    return -EBADF;

  auto fin = pcb->ftbl->at(in);
  if (!fin || !can_read(fin->flags))
    return -EBADF;

  size_t offset = fin->offset;
  if (offptr) {
    if (!copy_from_user(&offset, (void *) offptr, sizeof(size_t)))
      return -EFAULT;
  }

  char buf[1024];
  int before = fin->seek(offset, file::begin);

  size_t written = 0;
  while (len >= 0) {
    long read = fin->read(buf, min(len, 1024l));
    if (read < 0)
      return written ? written : read;
    // EOF.
    if (read == 0)
      break;

    long ret = fout->write(buf, min(len, read));
    if (ret < 0)
      return written ? written : ret;
    
    len -= read;
    written += read;
  }

  // We don't modify file position when offset is specified.
  if (offptr)
    fin->seek(before, file::begin);
  return written;
}

HANDLE(openat, dirfd, _path, flags, mode) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  return pcb->open_file_from(path->get(), dirfd, flags, mode);
}

HANDLE(close, fd) {
  return pcb->close_file(fd);
}

HANDLE(close_range, begin, end, flags) {
  if (begin > end)
    return -EINVAL;

  // Unshare the table first.
  if (flags & 2) {
    auto old = pcb->ftbl;
    pcb->ftbl = new process_file_table(*old);
    pcb->ftbl->ref();
    old->drop();
  }

  // Mark the files as CLOEXEC, rather than closing them immediately.
  if (flags & 4) {
    for (int i = begin; i <= end; i++) {
      if (pcb->ftbl->count(i))
        pcb->ftbl->set_desc(i, FD_CLOEXEC);
    }
    return 0;
  }

  for (int i = begin; i <= end; i++)
    pcb->close_file(i);
  return 0;
}

HANDLE(faccessat, dirfd, _path, mode) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  return detail::faccessat(dirfd, path->get(), mode, 0);
}

HANDLE(faccessat2, dirfd, _path, mode, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;
  if (flags & ~(AT_EACCESS | AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW))
    return -EINVAL;

  return detail::faccessat(dirfd, path->get(), mode, flags);
}

HANDLE(utimensat, dirfd, _path, times, flags) {
  bool follow = !(flags & AT_SYMLINK_NOFOLLOW);
  bool emptypath = flags & AT_EMPTY_PATH;

  auto path = copy_from_user((char *) _path);
  if (!path || ((!*path) && !emptypath))
    return -EFAULT;
  
  inode *node;
  if (!emptypath) {
    auto fd = pcb->obtain_file(path->get(), dirfd, O_PATH);
    if (!fd)
      return fd;

    node = (*fd)->node;
  } else if (dirfd != AT_FDCWD) {
    auto file = pcb->ftbl->at(dirfd);
    if (!file)
      return -EBADF;
    node = file->node();
  } else node = pcb->pwd->node;

  if (auto ret = writable(pcb->euid, pcb->egid, node); ret < 0)
    return ret;

  auto now = os::now();
  size_t atime = now, mtime = now;
  auto meta_before = node->get_meta();
  bool changed = false;
  if (times != 0) {
    timespec time[2];
    if (!copy_from_user(time, (void*) times, sizeof(timespec) * 2))
      return -EFAULT;
    if (time[0].tv_nsec >= long(1_s) || time[1].tv_nsec >= long(1_s))
      return -EINVAL;

    if (time[0].tv_nsec == UTIME_OMIT)
      atime = meta_before.atime;
    else if (time[0].tv_nsec != UTIME_NOW)
      changed = true, atime = time[0].tv_nsec + time[0].tv_sec * 1_s;

    if (time[1].tv_nsec == UTIME_OMIT)
      mtime = meta_before.mtime;
    else if (time[1].tv_nsec != UTIME_NOW)
      changed = true, mtime = time[1].tv_nsec + time[1].tv_sec * 1_s;
  }

  if (changed && node->uid != pcb->euid)
    return -EACCES;

  inode::meta meta(atime, now, mtime);
  node->set_meta(meta);
  return 0;
}

HANDLE(renameat2, olddirfd, oldpath, newdirfd, newpath, flags) {
  return detail::rename(olddirfd, oldpath, newdirfd, newpath, flags);
}

HANDLE(umask, mask) {
  int m = pcb->umask;
  pcb->umask = mask & 0777;
  return m;
}

HANDLE(readlinkat, dirfd, _path, buf, size) {
  if (size < 0)
    return -EINVAL;
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  auto fd = pcb->obtain_file(path->get(), dirfd, O_RDONLY | O_NOFOLLOW);
  if (!fd)
    return fd;
  auto link = (*fd)->node->readlink();
  if (!link)
    return -EINVAL;
  size = min(size, (long) link->size());
  if (!copy_to_user((void *) buf, link->c_str(), size))
    return -EFAULT;
  return size;
}

HANDLE(chdir, _path) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;
  auto fd = pcb->obtain_file(path->get(), AT_FDCWD, O_RDONLY);
  if (!fd)
    return fd;

  auto node = (*fd)->node;
  if (node->type != inode::Dir)
    return -ENOTDIR;

  pcb->pwd = *fd;
  return 0;
}

HANDLE(fchdir, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  if (file->node()->type != inode::Dir)
    return -ENOTDIR;
  pcb->pwd = file->entry;
  return 0;
}

HANDLE(fchown, fd, uid, gid) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  auto node = file->node();
  if (node->uid != pcb->euid && pcb->euid != 0)
    return -EPERM;
  if (uid != -1)
    node->uid = uid;
  if (gid != -1)
    node->gid = gid;
  
  auto ret = node->onchown();
  if (ret < 0)
    return ret;

  // We also need to clear setuid bits.
  node->mode &= ~04000;
  // setgid bit is cleared only when it is executable for a group.
  if ((node->mode & 02000) && (node->mode & 0010))
    node->mode &= ~02000;
  return 0;
}

HANDLE(fchownat, dirfd, _path, uid, gid, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;
  int openflags = 0;
  
  if (flags & AT_SYMLINK_NOFOLLOW)
    openflags |= O_NOFOLLOW;

  auto fd = pcb->obtain_file(path->get(), dirfd, O_PATH | openflags);
  if (!fd)
    return fd;

  auto node = (*fd)->node;
  if (node->uid != pcb->euid && pcb->euid != 0)
    return -EPERM;
  if (int(uid) != -1)
    node->uid = uid;
  if (int(gid) != -1)
    node->gid = gid;
  
  auto ret = node->onchown();
  if (ret < 0)
    return ret;

  // We also need to clear setuid bits.
  node->mode &= ~04000;
  // setgid bit is cleared only when it is executable for a group.
  if ((node->mode & 02000) && (node->mode & 0010))
    node->mode &= ~02000;
  return node->onchmod();
}

HANDLE(fchmod, fd, mode) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  if (mode > 0107777)
    return -EINVAL;

  auto node = file->node();
  if (node->uid != pcb->euid && pcb->euid != 0)
    return -EPERM;
  if (auto ret = writable(pcb->euid, pcb->egid, file->entry->parent->node); ret < 0)
    return ret;

  node->mode = mode;
  return node->onchmod();
}

HANDLE(fchmodat, dirfd, _path, mode, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  int openflags = 0;
  if (flags & AT_SYMLINK_NOFOLLOW)
    openflags |= O_NOFOLLOW;
  
  auto fd = pcb->obtain_file(path->get(), dirfd, O_PATH | openflags);
  if (!fd)
    return fd;

  auto node = (*fd)->node;
  if (node->uid != pcb->euid && pcb->euid != 0)
    return -EPERM;
  if (auto ret = writable(pcb->euid, pcb->egid, (*fd)->parent->node); ret < 0)
    return ret;

  node->mode = mode;
  auto ret = node->onchmod();
  return ret;
}

HANDLE(flock, fd, cmd) {
  auto f = pcb->ftbl->at(fd);
  if (!f)
    return -EBADF;

  bool noblock = cmd & LOCK_NB;
  cmd &= ~LOCK_NB;
  // The lock is unlocked. Wake up all processes hanging on it.
  if (cmd == LOCK_UN) {
    f->flockmode = 0;
    f->fwait.wake_all();
    return 0;
  }

  if (cmd != LOCK_SH && cmd != LOCK_EX)
    return -EINVAL;
  
  // The lock is acquired.
  if (f->flockmode == 0 || (cmd == LOCK_SH && f->flockmode == LOCK_SH)) {
    f->flockmode = cmd;
    return 0;
  }

  // The attempt to acquire the lock will cause a block.
  if (noblock)
    return -EAGAIN;

  // Do the real blocking.
  wait_entry entry;
  hangon(f->fwait, f->lock, entry);
  return 0;
}

// These two system calls only exist in RISC-V.
#ifdef RV
HANDLE(fstatat, dirfd, _path, buf, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;

  int openflags = 0;
  if (flags & AT_SYMLINK_NOFOLLOW)
    openflags |= O_NOFOLLOW;
  if (flags & AT_EMPTY_PATH)
    openflags |= O_EMPTYPATH;

  auto fd = pcb->obtain_file_emptyable(path->get(), dirfd, O_PATH | openflags);
  if (!fd)
    return fd;

  auto node = (*fd)->node;
  auto meta = node->get_meta();
  stat stat {
    .st_dev = 0,
    .st_ino = (unsigned long) node->inum(),
    .st_mode = (unsigned) (node->mode | ext_inode::fromtype(node->type)),
    .st_nlink = node->nlink(),
    .st_uid = (unsigned) node->uid,
    .st_gid = (unsigned) node->gid,
    .st_rdev = node->rdev(),
    .__pad = 0,
    .st_size = (long) node->size(),
    .st_blksize = 4096,
    .st_blocks = (long) (511 + node->size()) / 512,
    .st_atim = { .tv_sec = long(meta.atime / 1_s), .tv_nsec = long(meta.atime % 1_s) },
    .st_mtim = { .tv_sec = long(meta.mtime / 1_s), .tv_nsec = long(meta.mtime % 1_s) },
    .st_ctim = { .tv_sec = long(meta.ctime / 1_s), .tv_nsec = long(meta.ctime % 1_s) },
  };
  if (!copy_to_user((void *) buf, &stat, sizeof(stat)))
    return -EFAULT;
  return 0;
}

HANDLE(fstat, fd, buf) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  auto node = file->node();
  auto meta = node->get_meta();
  stat stat {
    .st_dev = 0,
    .st_ino = (unsigned long) node->inum(),
    .st_mode = (unsigned) (node->mode | ext_inode::fromtype(node->type)),
    .st_nlink = node->nlink(),
    .st_uid = (unsigned) node->uid,
    .st_gid = (unsigned) node->gid,
    .st_rdev = 0,
    .__pad = 0,
    .st_size = (long) node->size(),
    .st_blksize = 4096,
    .st_blocks = 0,
    .st_atim = { .tv_sec = long(meta.atime / 1_s), .tv_nsec = long(meta.atime % 1_s) },
    .st_mtim = { .tv_sec = long(meta.mtime / 1_s), .tv_nsec = long(meta.mtime % 1_s) },
    .st_ctim = { .tv_sec = long(meta.ctime / 1_s), .tv_nsec = long(meta.ctime % 1_s) },
  };
  if (!copy_to_user((void *) buf, &stat, sizeof(stat)))
    return -EFAULT;
  return 0;
}
#endif // #ifdef RV

HANDLE(sync, _) {
  for (auto fs : vfs::to_sync())
    fs->sync();
  return 0;
}

HANDLE(fsync, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  return file->node()->fs->sync();
}

HANDLE(fdatasync, fd) {
  // TODO: We still flush the entire file system.
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  return file->node()->fs->sync();
}

HANDLE(msync, addr, len, flags) {
  if (addr % PAGE_SIZE != 0)
    return -EINVAL;
  
  auto vma = pcb->vma->find(addr);
  if (!vma->backup || vma->end < (unsigned long) addr + len)
    return -ENOMEM;

  if (flags & MS_ASYNC && !(flags & MS_INVALIDATE))
    return 0;

  vma->backup->sync();
  return 0;
}

HANDLE(symlinkat, target, dirfd, linkpath) {
  auto path = copy_from_user((char *) linkpath);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  auto tgt = copy_from_user((char *) target);
  if (!tgt)
    return tgt;
  if (!*tgt)
    return -EFAULT;

  auto dirname = os::dirname(path->get());
  auto basename = os::basename(path->get());

  auto fd = pcb->obtain_file(dirname, dirfd, O_PATH);
  if (!fd)
    return fd;

  auto node = (*fd)->node;
  if (auto ret = writable(pcb->euid, pcb->egid, node); ret < 0)
    return ret;

  // Do we really need to look up twice?
  if (node->lookup(basename))
    return -EEXIST;

  if (auto ret = node->create(basename, inode::Link, 0666 & ~pcb->umask); ret < 0)
    return ret;

  auto inode = node->lookup(basename);
  inode->write(0, tgt->get(), strlen(tgt->get()), 0);
  return 0;
}

HANDLE(unlinkat, dirfd, _path, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  auto fd = pcb->obtain_file(path->get(), dirfd, O_PATH);
  if (!fd)
    return fd;

  auto entry = *fd;

  auto dir = entry->parent->node;
  if (auto ret = writable(pcb->euid, pcb->egid, dir); ret < 0)
    return ret;

  bool rmdir = flags & AT_REMOVEDIR;
  if (rmdir) {
    if (entry->node->type != inode::Dir)
      return -ENOTDIR;

    if (int ret = dir->rmdir(entry->name); ret < 0)
      return ret;
    
    // Forget it in dcache.
    vfs::invalidate(entry->parent->node, entry->name);
    return 0;
  }

  if (entry->node->type == inode::Dir)
    return -EISDIR;

  // unlink() will check whether `dir` is indeed a dir.
  int ret = dir->unlink(entry->name);
  if (ret < 0)
    return ret;

  // Forget it in dcache.
  vfs::invalidate(entry->parent->node, entry->name);
  return 0;
}

HANDLE(brk, addr) {
  return pcb->vma->brk(addr);
}

HANDLE(dup, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  // Increase pipe reader/writer counts.
  if (auto node = dyn_cast<pipe_inode>(file->node()))
    node->incf(file);
  
  if (pcb->ftbl->size() >= pcb->rlims[RLIMIT_OFILE].rlim_cur)
    return -EMFILE;

  return pcb->ftbl->allocate(file);
}

HANDLE(dup3, oldfd, newfd, flags) {
  if (oldfd == newfd)
    return -EINVAL;

  auto file = pcb->ftbl->at(oldfd);
  if (!file || newfd < 0 || (unsigned long) newfd >= pcb->rlims[RLIMIT_OFILE].rlim_cur)
    return -EBADF;

  // Currently `flags` can only be these two values.
  if (flags && flags != O_CLOEXEC)
    return -EINVAL;

  if (auto node = dyn_cast<pipe_inode>(file->node()))
    node->incf(file);

  pcb->ftbl->allocate(file, newfd);
  int desc = flags & O_CLOEXEC ? FD_CLOEXEC : 0;
  pcb->ftbl->set_desc(newfd, desc);
  return newfd;
}

HANDLE(getpid, _) {
  return pcb->pid;
}

HANDLE(getppid, _) {
  return pcb->parent->pid;
}

HANDLE(getuid, _) {
  return pcb->uid;
}

HANDLE(setuid, uid) {
  if (uid < 0)
    return -EINVAL;

  if (pcb->euid == 0) {
    pcb->uid = pcb->euid = pcb->suid = uid;
    return 0;
  }

  if (uid != pcb->suid && uid != pcb->uid && uid != pcb->euid)
    return -EPERM;
  
  pcb->euid = pcb->suid = uid;
  return 0;
}

HANDLE(geteuid, _) {
  return pcb->euid;
}

HANDLE(setreuid, _ruid, _euid) {
  // We must convert them to int.
  int ruid = _ruid, euid = _euid;

  if (ruid < -1 || euid < -1)
    return -EINVAL;
  // Nothing changes.
  if (ruid == -1 && euid == -1)
    return 0;

  if (ruid != -1 && pcb->euid != 0 && ruid != pcb->suid && ruid != pcb->uid && ruid != pcb->euid)
    return -EPERM;
  if (euid != -1 && pcb->euid != 0 && euid != pcb->suid && euid != pcb->uid && euid != pcb->euid)
    return -EPERM;
  
  if (ruid != -1)
    pcb->uid = ruid;
  if (euid != -1)
    pcb->euid = euid;

  pcb->suid = pcb->euid;
  return 0;
}

HANDLE(getresuid, ruid, euid, suid) {
  if (!copy_to_user((void *) ruid, &pcb->uid, sizeof(pcb->uid)))
    return -EFAULT;
  if (!copy_to_user((void *) euid, &pcb->euid, sizeof(pcb->euid)))
    return -EFAULT;
  if (!copy_to_user((void *) suid, &pcb->suid, sizeof(pcb->suid)))
    return -EFAULT;
  return 0;
}

HANDLE(setresuid, _ruid, _euid, _suid) {
  // We must convert them to int.
  int ruid = _ruid, euid = _euid, suid = _suid;

  if (ruid < -1 || euid < -1 || suid < -1)
    return -EINVAL;
  // Nothing changes.
  if (ruid == -1 && euid == -1 && suid == -1)
    return 0;

  if (ruid != -1 && pcb->euid != 0 && ruid != pcb->suid && ruid != pcb->uid && ruid != pcb->euid)
    return -EPERM;
  if (euid != -1 && pcb->euid != 0 && euid != pcb->suid && euid != pcb->uid && euid != pcb->euid)
    return -EPERM;
  if (suid != -1 && pcb->euid != 0 && suid != pcb->suid && suid != pcb->uid && suid != pcb->euid)
    return -EPERM;
  
  if (ruid != -1)
    pcb->uid = ruid;
  if (euid != -1)
    pcb->euid = euid;
  if (suid != -1)
    pcb->suid = suid;

  return 0;
}

HANDLE(getegid, _) {
  return pcb->egid;
}

HANDLE(setgid, gid) {
  if (gid < 0)
    return -EINVAL;

  if (pcb->gid == 0) {
    pcb->gid = pcb->egid = pcb->sgid = gid;
    return 0;
  }

  if (gid != pcb->sgid && gid != pcb->gid && gid != pcb->egid)
    return -EPERM;
  
  pcb->egid = pcb->sgid = gid;
  return 0;
}

HANDLE(setregid, rgid, egid) {
  if (rgid < -1 || egid < -1)
    return -EINVAL;
  // Nothing changes.
  if (rgid == -1 && egid == -1)
    return 0;

  if (rgid != -1 && pcb->euid != 0 && rgid != pcb->sgid && rgid != pcb->gid && rgid != pcb->egid)
    return -EPERM;
  if (egid != -1 && pcb->euid != 0 && egid != pcb->sgid && egid != pcb->gid && egid != pcb->egid)
    return -EPERM;
  
  if (rgid != -1)
    pcb->gid = rgid;
  if (egid != -1)
    pcb->egid = egid;

  pcb->sgid = pcb->egid;
  return 0;
}

HANDLE(setresgid, rgid, egid, sgid) {
  if (rgid < -1 || egid < -1 || sgid < -1)
    return -EINVAL;
  // Nothing changes.
  if (rgid == -1 && egid == -1 && sgid == -1)
    return 0;

  if (rgid != -1 && pcb->euid != 0 && rgid != pcb->sgid && rgid != pcb->gid && rgid != pcb->egid)
    return -EPERM;
  if (egid != -1 && pcb->euid != 0 && egid != pcb->sgid && egid != pcb->gid && egid != pcb->egid)
    return -EPERM;
  if (sgid != -1 && pcb->euid != 0 && sgid != pcb->sgid && sgid != pcb->gid && sgid != pcb->egid)
    return -EPERM;
  
  if (rgid != -1)
    pcb->gid = rgid;
  if (egid != -1)
    pcb->egid = egid;
  if (sgid != -1)
    pcb->sgid = sgid;

  return 0;
}

HANDLE(getgid, _) {
  return pcb->gid;
}

HANDLE(gettid, _) {
  return tcb->tid;
}

HANDLE(getsid, pid) {
  if (pid == 0)
    return pcb->sid;

  auto proc = pidmap->find(pid);
  if (proc == pidmap->end())
    return -ESRCH;

  return (*proc).second->sid;
}

HANDLE(setsid, _) {
  if (pcb->sid == pcb->pid)
    return -EPERM;

  pcb->sid = pcb->pid;
  return 0;
}

HANDLE(set_tid_address, tidaddr) {
  tcb->ctidaddr = (void *) tidaddr;
  return tcb->tid;
}

HANDLE(getrandom, buf, len, flags) {
  // We deliberately ignore flags here.
  // We set /dev/random and /dev/urandom to point to the same thing, so GRND_RANDOM would be safe to ignore.
  // Moreover, it never blocks (we should have obtained enough entropy on boot),
  // so this is also alright.
  unsigned block[16];
  long read = 0;
  while (len > 0) {
    auto l = min(len, 64l);
    random->read(0, block, l, 0);
    if (!copy_to_user((void *) buf, block, l))
      return -EFAULT;
    len -= l;
    read += l;
  }
  return read;
}

// We only have a single CPU.
HANDLE(sched_getaffinity, pid, size, mask) {
  if (size < (int) sizeof(unsigned long))
    return -EINVAL;

  if (!copy_to_user((void *) mask, zeroes, size))
    return -EFAULT;
  char kset = 1;
  if (!copy_to_user((void *) mask, &kset, 1))
    return -EFAULT;
  return 0;
}

HANDLE(sched_setaffinity, pid, size, _mask) {
  if (size < 4)
    return -EINVAL;
  
  
  int mask;
  if (!copy_from_user(&mask, (void *) _mask, sizeof(int)))
    return -EFAULT;

  if (mask != 1)
    return -EINVAL;
  return 0;
}

// We don't (and won't) have NUMA. Always return default value.
HANDLE(get_mempolicy, policy, nmask, maxnode, addr, flags) {
  long zero = 0;
  if (policy) {
    if (!copy_to_user((void *) policy, &zero, 4))
      return -EFAULT;
  }

  if (nmask && maxnode > 0)
    if (!copy_to_user((void *) nmask, &zero, 8))
      return -EFAULT;

  return 0;
}

HANDLE(capget, header, data) {
  cap_header h;
  cap_data dat;

  if (!header)
    return -EFAULT;

  if (!copy_from_user(&h, (void *) header, sizeof(h)))
    return -EFAULT;

  // If version is 0, user is asking what we support.
  if (h.version == 0) {
    h.version = LINUX_CAPABILITY_VERSION_3;
    h.pid = 0;
    if (!copy_to_user((void *) header, &h, sizeof(h)))
      return -EFAULT;
    return 0;
  }

  if (h.version != LINUX_CAPABILITY_VERSION_3)
    return -EINVAL;

  if (!data)
    return -EFAULT;

  // TODO: stub: grant everything.
  dat.effective   = 0xffffffff;
  dat.permitted   = 0xffffffff;
  dat.inheritable = 0xffffffff;
  if (!copy_to_user((void *) data, &dat, sizeof(dat))) return -EFAULT;
  return 0;
}

HANDLE(setpgid, pid, pgid) {
  if (pid == 0)
    pid = pcb->pid;
  if (pgid == 0)
    pgid = pcb->pid;
  
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
  if (pid == 0)
    pid = pcb->pid;

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
  if (!copy_to_user((void*) buf, path.c_str(), path.size() + 1))
    return -EFAULT;
  return buf;
}

HANDLE(uname, buf) {
  // Just to bypass glibc kernel version checks.
  // Linux 6.18 is actually the latest one.
  utsname name {
    .sysname = "Linux",
    .nodename = "",
    .release = "6.18.0",
    .version = "#1 Dec 22 2025",
    .machine = "riscv64",
  };
  auto host = hostname();
  memcpy(name.nodename, host, strlen(host));
  if (!copy_to_user((void *) buf, &name, sizeof(utsname)))
    return -EFAULT;
  return 0;
}

HANDLE(get_robust_list, tid, headptr, size) {
  if (tid == 0)
    tid = tcb->tid;
  
  if (!tidmap->count(tid))
    return -ESRCH;

  auto thread = (*tidmap)[tid];
  size_t sz = sizeof(void*);
  if (!copy_to_user((void *) headptr, &thread->robust_list, sz))
    return -EFAULT;

  if (!copy_to_user((void *) size, &sz, sizeof(sz)))
    return -EFAULT;
  
  return 0;
}

HANDLE(set_robust_list, headptr, size) {
  // Disable robust lists for now. Debug for it later (TODO)
  return -ENOSYS;

  if (size != sizeof(robust_list_head))
    return -EINVAL;
  
  tcb->robust_list = (void *) headptr;
  return 0;
}

HANDLE(rseq, _) {
  return -ENOSYS;
}

#ifdef RV
HANDLE(riscv_hwprobe, _) {
  return -ENOSYS;
}
#endif

// This system call returns in clock ticks.
HANDLE(times, buf) {
  long utime = 0, stime = 0;
  for (auto t : pcb->threads) {
    utime += t->ruse.ru_utime;
    stime += t->ruse.ru_stime;
  }
  tms times {
    .tms_utime = utime,
    .tms_stime = stime,
    .tms_cutime = pcb->cruse.ru_utime,
    .tms_cstime = pcb->cruse.ru_stime,
  };
  if (!copy_to_user((void *) buf, &times, sizeof(tms)))
    return -EFAULT;
  return clock_period;
}

HANDLE(getrusage, who, buf) {
  switch (who) {
  case -1: { // Children
    rusage v = (rusage) pcb->cruse;
    if (!copy_to_user((void *) buf, &v, sizeof(rusage)))
      return -EFAULT;
    return 0;
  }
  case 0: { // Self
    pusage use {};
    for (auto t : pcb->threads)
      use += t->ruse;
    rusage v = (rusage) use;
    if (!copy_to_user((void *) buf, &v, sizeof(rusage)))
      return -EFAULT;
    return 0;
  }
  case 1: {// Thread
    rusage v = (rusage) tcb->ruse;
    if (!copy_to_user((void *) buf, &v, sizeof(rusage)))
      return -EFAULT;
    return 0;
  }
  }
  return -EINVAL;
}

HANDLE(clone, flags, stack, parenttid, tls, childtid) {
  return detail::clone(flags, stack, (void *) parenttid, (void *) tls, (void *) childtid);
}

HANDLE(clone3, _args, size) {
  clone_args args;
  if (size < (int) sizeof(clone_args))
    return -EINVAL;

  if (!copy_from_user(&args, (void *) _args, size))
    return -EFAULT;

  if ((args.flags & 0xff) != 0)
    return -EINVAL;

  // The stack is the lowest byte; we need to advance it.
  // The two fields must either be both zero, or both non-zero.
  if (!!args.stack ^ !!args.stack_size)
    return -EINVAL;

  va_t stack = args.stack + args.stack_size;
  return detail::clone(args.flags | args.exit_signal, stack, (void *) args.parent_tid, (void *) args.tls, (void *) args.child_tid);
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
  os::terminate(tcb, ret, false);
  panic("exit: unreachable");
}

HANDLE(exit_group, ret) {
  os::terminate(pcb, ret, false);
  panic("exit_group: unreachable");
}

HANDLE(fcntl, fd, ty, args) {
  return detail::fcntl(fd, ty, args);
}

HANDLE(getdents64, fd, dirents, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;
  if (file->node()->type != inode::Dir)
    return -ENOTDIR;
  auto items = file->node()->list();
  
  char *pos = (char *) dirents;
  if (cnt < (int) sizeof(linux_dirent64))
    return -EINVAL;
  constexpr unsigned nameoff = offsetof(linux_dirent64, name);
  for (unsigned i = file->offset; i < items.size(); i++) {
    const auto &item = items[i];
    // Maintain alignment.
    unsigned short len = roundup<8>(nameoff + item.name.size() + 1);
    if (va_t(pos) - dirents + len >= va_t(cnt))
      break;

    unsigned char type = inode::as_dt(item.ty);
    linux_dirent64 entry { .inum = (unsigned long) item.inum, ._resv = 0, .len = len, .type = type };
    if (!copy_to_user(pos, &entry, nameoff))
      return -EFAULT;
    if (!copy_to_user(pos + nameoff, item.name.c_str(), item.name.size() + 1))
      return -EFAULT;
    pos += len;
    file->offset++;
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

  (void) data;
  auto ret = detail::mount(src->get(), tgt->get(), fsty->get(), flags);
  asm volatile("fence iorw, iorw" ::: "memory");
  return ret;
}

HANDLE(umount2, target, flags) {
  auto path = copy_from_user((char *) target);
  if (!path)
    return path;

  if ((flags & MNT_DETACH )|| (flags & MNT_EXPIRE)) {
    printk("umount: unknown flags %d\n", flags);
    return -EINVAL;
  }

  auto pentry = pcb->vfs->lookup_from(path->get(), pcb->pwd, !(flags & UMOUNT_NOFOLLOW), false);
  if (!pentry)
    return -pentry;

  auto entry = *pentry;
  if (!entry->mnt)
    return -EINVAL;
  return pcb->vfs->unmount(entry->mnt, flags);
}

HANDLE(chroot, _path) {
  // Only root can chroot.
  if (pcb->euid != 0)
    return -EACCES;

  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;
  if (!*path)
    return -EFAULT;

  auto dentry = pcb->vfs->lookup_from(path->get(), pcb->pwd);
  if (!dentry)
    return dentry;
  if ((*dentry)->node->type != inode::Dir)
    return -ENOTDIR;

  return pcb->vfs->chroot(*dentry);
}

HANDLE(prlimit64, pid, resource, new_rlim, old_rlim) {
  return detail::prlimit64(pid, resource, (void *) new_rlim, (void *) old_rlim);
}

HANDLE(getrlimit, lim, rlim) {
  if (lim < 0 || lim >= 16)
    return -EINVAL;
  if (!copy_to_user((void *) rlim, &pcb->rlims[lim], sizeof(rlimit)))
    return -EFAULT;
  return 0;
}

HANDLE(ioctl, fd, op, argp) {
  return detail::ioctl(fd, op, (void *) argp);
}

HANDLE(clock_gettime, id, tp) {
  timespec spec;
  size_t time = rdtime() * clock_period;

  switch (id) {
  case CLOCK_MONOTONIC:
  case CLOCK_MONOTONIC_RAW: // We don't have NTP now.
    break; // Do nothing
  case CLOCK_REALTIME:
  case CLOCK_REALTIME_COARSE:
    time += realtime;
    break;
  default:
    return -EINVAL;
  }

  spec.tv_sec = time / 1_s;
  spec.tv_nsec = time % 1_s;
  if (!copy_to_user((void *) tp, &spec, sizeof(timespec)))
    return -EFAULT;
  return 0;
}

HANDLE(clock_settime, id, tp) {
  timespec ts;
  if (!copy_from_user(&ts, (void *) tp, sizeof(timespec)))
    return -EFAULT;

  switch (id) {
  case CLOCK_REALTIME: {
    unsigned long time = ts.tv_nsec + ts.tv_sec * 1_s;
    unsigned long tick = rdtime() * (unsigned long) clock_period;
    if (time < tick)
      return -EINVAL;

    auto nreal = time - tick;
    // Advance timeout for all threads that wait on a real time clock.
    for (auto entry : napping.q) {
      if (entry->tcb->sclock == CLOCK_REALTIME)
        entry->tcb->timeout += (realtime - nreal + tick_length - 1) / tick_length;
    }
    realtime = nreal;
    return 0;
  }
  default:
    return -EINVAL;
  }
}

HANDLE(clock_getres, id, tp) {
  // We only have one time source for now.
  if (id < 0)
    return -EINVAL;
  if (!tp)
    return 0;

  timespec res {
    .tv_sec = 0,
    .tv_nsec = clock_period
  };
  if (!copy_to_user((void *) tp, &res, sizeof(timespec)))
    return -EFAULT;
  return 0;
}

HANDLE(gettimeofday, tv, tz) {
  if (tz)
    if (!copy_to_user((void*) tz, &zone, sizeof(timezone)))
      return -EFAULT;
  
  if (tv) {
    auto cur = now();
    timeval ts {
      .tv_sec = long(cur / 1_s),
      .tv_usec = long(cur % 1_s) / 1000,
    };
    if (!copy_to_user((void*) tv, &ts, sizeof(timeval)))
      return -EFAULT;
  }
  return 0;
}

HANDLE(settimeofday, tv, tz) {
  if (tz)
    // TODO: What is a valid timezone anyway?
    return -EINVAL;
  if (tv) {
    // Note this is a timeval, rather than a timespec.
    timeval ts;
    if (!copy_from_user(&ts, (void*) tv, sizeof(timeval)))
      return -EFAULT;
    
    unsigned long time = ts.tv_usec * 1_us + ts.tv_sec * 1_s;
    unsigned long tick = rdtime() * (unsigned long) clock_period;
    if (time < tick)
      return -EINVAL;

    auto nreal = time - tick;
    // Advance timeout for all threads that wait on a real time clock.
    for (auto entry : napping.q) {
      if (entry->tcb->sclock == CLOCK_REALTIME)
        entry->tcb->timeout += (realtime - nreal + tick_length - 1) / tick_length;
    }
    realtime = nreal;
  }
  return 0;
}

HANDLE(mmap, addr, len, prot, flags, fd, offset) {
  return detail::mmap(addr, len, prot, flags, fd, offset);
}

HANDLE(mprotect, start, len, prot) {
  // Find the VMA that contains this mprotect.
  return detail::mprotect(start, len, prot);
}

HANDLE(munmap, addr, len) {
  return detail::munmap(addr, len);
}

HANDLE(shmget, key, len, flags) {
  return detail::shmget(key, len, flags);
}

HANDLE(shmat, key, addr, flags) {
  return detail::shmat(key, addr, flags);
}

HANDLE(shmdt, addr) {
  return detail::shmdt(addr);
}

HANDLE(shmctl, key, op, buf) {
  return detail::shmctl(key, op, (void *) buf);
}

HANDLE(madvise, addr, len, type) {
  if (len < 0 || addr % PAGE_SIZE != 0)
    return -EINVAL;

  // TODO: listen to advice.
  // This is purely performance-related so we can probably ignore it.
  return 0;
}

HANDLE(sched_yield, _) {
  // There is no system call context when we directly call yield().
  scheduler.yield();
  // noreturn
}

HANDLE(rt_sigprocmask, how, set, oldset, size) {
  if (oldset)
    if (!copy_to_user((void *) oldset, &tcb->mask.sig, size)) return -EFAULT;
  if (!set)
    return 0;

  sigset_t sigset;
  if (!copy_from_user(&sigset, (void *) set, size))
    return -EFAULT;
  if (size > 8)
    return -EINVAL;

  unsigned long mask = sigset.val;
  // In Linux, the bit for `sig` is usually `sig - 1`.
  mask <<= 1;

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

HANDLE(rt_sigtimedwait, sig, info, timeout) {
  if (info) {
    printk("sigtimedwait: no info yet\n");
    return -EINVAL;
  }

  sigset_t waitset;
  if (!copy_from_user(&waitset, (void *) sig, sizeof(sigset_t)))
    return -EFAULT;

  size_t tm = 0;
  if (timeout) {
    timespec time;
    if (!copy_from_user(&time, (void *) timeout, sizeof(timespec)))
      return -EFAULT;

    tm = time.tv_nsec + time.tv_sec * 1_s;
  }

  auto wait = waitset.val;
  // In Linux, the bit is usually `sig - 1`.
  wait <<= 1;
  if (tm == 0) {
    if (wait & tcb->pending.sig)
      return tcb->pending.next(~wait);
    return -EAGAIN;
  }

  tcb->sigresume = -2;
  tcb->sigwait = wait;
  tcb->sleep(tm);
  
  auto ret = tcb->sigresume != -2 ? tcb->sigresume : -EAGAIN;
  tcb->sigresume = 0;
  return ret;
}

HANDLE(rt_sigreturn, _) {
  auto trap = (trapframe *) tcb->ksp;
  memcpy(trap, &tcb->sigf, sizeof(trapframe));
  // We shouldn't change the value of a0.
  return trap->regs[8];
}

HANDLE(kill, pid, sig) {
  if (sig >= 64)
    return -EINVAL;
  
  if (pid < -1) {
    // Send to every process in the process group. (TODO)
    pid = abs(pid);
  }

  if (pid == 0)
    pid = pcb->pid;
  
  auto fproc = pidmap->find(pid);
  if (fproc == pidmap->end())
    return -ESRCH;
  auto [_, proc] = *fproc;
  
  if (proc->uid != pcb->uid && proc->uid != pcb->euid && pcb->euid != 0)
    return -EPERM;

  proc->send_signal(sig);
  return 0;
}

HANDLE(tgkill, pid, tid, sig) {
  if (sig >= 64)
    return -EINVAL;
  if (pid == 0)
    pid = pcb->pid;

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

HANDLE(pselect6, maxcnt, _read, _write, _except, tmo, sigmask) {
  size_t timeout = 1.8e19;
  if (tmo) {
    timespec t;
    if (!copy_from_user(&t, (void *) tmo, sizeof(timespec)))
      return -EFAULT;
    if (t.tv_nsec < 0 || t.tv_nsec > 999'999'999)
      return -EINVAL;
    timeout = t.tv_nsec + t.tv_sec * 1_s;
  }
  if (maxcnt <= 0 || maxcnt >= 1024)
    return -EINVAL;

  fd_set read {}, write {}, except {};
  if (_read && !copy_from_user(&read, (void *) _read, sizeof(fd_set)))
    return -EFAULT;
  if (_write && !copy_from_user(&write, (void *) _write, sizeof(fd_set)))
    return -EFAULT;
  if (_except && !copy_from_user(&except, (void *) _except, sizeof(fd_set)))
    return -EFAULT;

  // sizeof(pollfd) == 8, so this is an 1 KB buffer.
  pollfd _fds[128];
  pollfd *fds;
  [[likely]]
  if (maxcnt < 128)
    fds = _fds;
  else
    fds = new pollfd[maxcnt];
  
  int cnt = 0;
  for (int i = 0; i < maxcnt; i++) {
    int events = 0;
    if (FD_ISSET(i, &read))
      events |= POLLIN;
    if (FD_ISSET(i, &write))
      events |= POLLOUT;
    
    if (events) {
      fds[cnt].fd = i;
      fds[cnt].events = events;
      cnt++;
    }
  }

  auto ret = detail::ppoll(fds, cnt, timeout, (void *) sigmask, false);
  if (ret >= 0) {
    fd_set read {}, write {}, except {};
    for (int i = 0; i < cnt; i++) {
      if (fds[i].revents & POLLIN)
        FD_SET(fds[i].fd, &read);
      if (fds[i].revents & POLLOUT)
        FD_SET(fds[i].fd, &write);
      if (fds[i].revents & POLLPRI)
        FD_SET(fds[i].fd, &except);
    }
    
    if (_read && !copy_to_user((void *) _read, &read, sizeof(fd_set)))
      return -EFAULT;
    if (_write && !copy_to_user((void *) _write, &write, sizeof(fd_set)))
      return -EFAULT;
    if (_except && !copy_to_user((void *) _except, &except, sizeof(fd_set)))
      return -EFAULT;
  }
  [[unlikely]] if (maxcnt > 128)
    delete[] fds;
  return ret;
}

HANDLE(ppoll, _fds, cnt, tmo, sigmask) {
  size_t timeout = 1.8e19;
  if (tmo) {
    timespec t;
    if (!copy_from_user(&t, (void *) tmo, sizeof(timespec)))
      return -EFAULT;
    if (t.tv_nsec < 0 || t.tv_nsec > 999'999'999)
      return -EINVAL;
    timeout = t.tv_nsec + t.tv_sec * 1_s;
  }

  return detail::ppoll((void *) _fds, cnt, timeout, (void *) sigmask, true);
}

HANDLE(nanosleep, rqtp, rmtp) {
  return detail::nanosleep(CLOCK_MONOTONIC, 0, (void *) rqtp, (void *) rmtp);
}

HANDLE(clock_nanosleep, clock, flags, rqtp, rmtp) {
  return detail::nanosleep(clock, flags, (void *) rqtp, (void *) rmtp);
}

HANDLE(rt_sigaction, sig, act, oldact) {
  if (sig <= 0 || sig >= 64)
    return -EINVAL;

  if (oldact) {
    os::sigaction a = pcb->actor->sigact[sig];
    ::sigaction v {
      .sa_handler = a.handler,
      .sa_flags = a.flags,
      .sa_mask = { a.mask.sig },
    };
    if (!copy_to_user((void *) oldact, &v, sizeof(::sigaction)))
      return -EFAULT;
  }
  if (act) {
    ::sigaction sigact;
    if (!copy_from_user(&sigact, (void *) act, sizeof(::sigaction)))
      return -EFAULT;
    
    os::sigaction a {
      .handler = sigact.sa_handler,
      .mask = sigact.sa_mask.val,
      .flags = sigact.sa_flags
    };
    pcb->actor->sigact[sig] = a;
  }
  return 0;
}

HANDLE(futex, uaddr, op, val, timeout, val2, val3) {
  return detail::futex((void *) uaddr, op, val, (void *) timeout, val2, val3);
}

HANDLE(wait4, pid, wstatus, options, rusage) {
  return detail::wait(pid, (void *) wstatus, options, (void *) rusage);
}

HANDLE(reboot, magic, magic2, op, arg) {
  if (int(magic) != int(0xfee1dead))
    return -EINVAL;
  if (magic2 != 0x28121969 && magic2 != 0x05121996 && magic2 != 0x16041998 && magic2 != 0x20112000)
    return -EINVAL;
  if (int(op) != int(0xcdef0123))
    printk("reboot: unsupported op: %d\n", int(op));
  sbi_system_reset();
}

HANDLE(pipe2, fds, flags) {
  int extra = 0;
  if (flags & O_CLOEXEC) extra |= O_CLOEXEC;
  if (flags & O_NONBLOCK) extra |= O_NONBLOCK;
  if (flags & O_DIRECT)
    return -EINVAL;

  // We will open 2 new files, so we have to check it here.
  if (pcb->ftbl->size() + 2 > pcb->rlims[RLIMIT_OFILE].rlim_cur)
    return -EMFILE;

  auto pipe = pipefs->get(); // On creation it has a writer and a reader, so don't increment.

  file *read = new file(new dentry("<pipe r>", pipe, nullptr), O_RDONLY | extra);
  file *write = new file(new dentry("<pipe w>", pipe, nullptr), O_WRONLY | extra);

  int fd[2] = { pcb->ftbl->allocate(read), pcb->ftbl->allocate(write) };
  if (!copy_to_user((void*) fds, fd, sizeof(fd)))
    return -EFAULT;
  return 0;
}

HANDLE(socket, domain, type, protocol) {
  return detail::socket(domain, type, protocol);
}

HANDLE(bind, fd, sockaddr, size) {
  return detail::bind(fd, (void *) sockaddr, size);
}

HANDLE(connect, fd, sockaddr, size) {
  return detail::connect(fd, (void *) sockaddr, size);
}

HANDLE(setsockopt, fd, level, optname, optval, optlen) {
  return detail::setsockopt(fd, level, optname, (void *) optval, optlen);
}

HANDLE(getsockname, fd, sock, len) {
  return detail::getsockname(fd, (void *) sock, (void *) len);
}

HANDLE(sendto, fd, buf, size, flags, dest, addrlen) {
  return detail::sendto(fd, (void *) buf, size, flags, (void *) dest, addrlen);
}

HANDLE(sendmsg, fd, msg, flags) {
  return detail::sendmsg(fd, (void *) msg, flags);
}

HANDLE(sendmmsg, fd, msg, n, flags) {
  int i = 0;
  for (; i < n; i++) {
    mmsghdr message;
    if (!copy_from_user(&message, (mmsghdr *) msg + i, sizeof(mmsghdr)))
      return -EFAULT;

    int sent = detail::sendmsg(fd, message.msg_hdr, flags);
    if (sent < 0)
      return i ? i : sent;
    
    if (!copy_to_user((void*) (msg + sizeof(mmsghdr) * i + offsetof(mmsghdr, msg_len)), &sent, sizeof(unsigned))) return -EFAULT;
  }
  return i;
}

HANDLE(recvfrom, fd, buf, size, flags, src, addrlen) {
  return detail::recvfrom(fd, (void *) buf, size, flags, (void *) src, addrlen);
}

HANDLE(syslog, type, buf, size) {
  return detail::syslog(type, (char *) buf, size);
}

HANDLE(sysinfo, info) {
  struct sysinfo sysinfo {
    .uptime = long((now() - realtime) / 1_s),
    .loads = {}, // TODO: what's this?
    .totalram = ptotal() * PAGE_SIZE,
    .freeram = pavail() * PAGE_SIZE,
    .sharedram = pshared,
    .bufferram = 0,
    .totalswap = 0,
    .freeswap = 0,
    .procs = (unsigned short) pidmap->size(),
    .totalhigh = 0,
    .freehigh = 0,
    .mem_unit = PAGE_SIZE
  };
  if (!copy_to_user((void *) info, &sysinfo, sizeof(struct sysinfo))) return -EFAULT;
  return 0;
}

HANDLE(setitimer, which, timer, old) {
  if (which < 0 || which >= 3)
    return -EINVAL;
  if (which != ITIMER_REAL)
    printk("setitimer: no timer %d yet\n", which);
  
  itimerval time;
  if (!copy_from_user(&time, (void *) timer, sizeof(itimerval)))
    return -EFAULT;
  if (time.it_interval.tv_usec > 999999 || time.it_interval.tv_usec < 0)
    return -EINVAL;
  if (time.it_value.tv_usec > 999999 || time.it_value.tv_usec < 0)
    return -EINVAL;
  
  auto intv = time.it_interval.tv_usec * 1000 + time.it_interval.tv_sec * 1_s;
  auto tm = time.it_value.tv_usec * 1000 + time.it_value.tv_sec * 1_s;

  if (old) {
    auto timer = pcb->itimers[which];
    auto intv = timer.interval * tick_length;
    auto time = timer.timeout * tick_length;
    itimerval v {
      .it_interval = { .tv_sec = long(intv / 1_s), .tv_usec = long(intv % 1_s) / 1000 },
      .it_value    = { .tv_sec = long(time / 1_s), .tv_usec = long(time % 1_s) / 1000 }
    };
    if (!copy_to_user((void *) old, &v, sizeof(itimerval)))
      return -EFAULT;
  }

  pcb->itimers[which].interval = (intv + tick_length - 1) / tick_length;
  pcb->itimers[which].timeout = (tm + tick_length - 1) / tick_length;
  scheduler.record_itimer_real(pcb);
  return 0;
}

HANDLE(getitimer, which, old) {
  if (which < 0 || which >= 3)
    return -EINVAL;

  auto timer = pcb->itimers[which];
  auto intv = timer.interval * tick_length;
  auto time = timer.timeout * tick_length;
  itimerval v {
    .it_interval = { .tv_sec = long(intv / 1_s), .tv_usec = long(intv % 1_s) / 1000 },
    .it_value    = { .tv_sec = long(time / 1_s), .tv_usec = long(time % 1_s) / 1000 }
  };
  if (!copy_to_user((void *) old, &v, sizeof(timeval) * 2))
    return -EFAULT;
  return 0;
}

SYSHANDLE_END

}

namespace os {

[[gnu::no_instrument_function]] void interrupt_handler(reg_t scause, reg_t stval, void *sepc) {
#ifdef RV
  reg_t sstatus;
  CSRR(sstatus, sstatus);
  bool from_kernel = sstatus & (1 << 8);
#endif
#ifdef LA
  bool from_kernel = true; // Placeholder for now
#endif

  auto tcb = active();
  auto pcb = tcb->pcb;
  // Switch from user mode to kernel mode.
  // We update kmode.
  if (!tcb->kmode) {
    size_t time = now();
    tcb->ruse.ru_utime += time - tcb->last_sched;
    tcb->last_sched = time;
    tcb->kmode = true;
  }

  if (scause < 0) {
    int kind = scause & 0xff;
    switch (kind) {
    case 5: { // Timer interrupt
      // Tick every 100ms.
      sbi_set_timer(rdtime() + tick_length / clock_period);
      scheduler.tick();
      scheduler.yield(); // TODO: check time slice
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
    case 2: // Illegal instruction
      printk("exception: illegal instruction (%p) when executing %p\n", *(unsigned*) sepc, sepc);
      break;
    case 4: // Load address misaligned
      printk("exception: load address misaligned at %p when executing %p\n", stval, sepc);
      break;
    case 5: // Load access fault
      printk("exception: load access fault at %p when executing %p\n", stval, sepc);
      break;
    case 7: // Store access fault
      printk("exception: store access fault at %p when executing %p\n", stval, sepc);
      break;
    case 12: // Instruction page fault
      printk("exception: instruction page fault at %p when executing %p (flags: %x)\n", stval, sepc, pte_flags(stval));
      break;
    case 13: // Load page fault
      printk("exception: load page fault at %p when executing %p\n", stval, sepc);
      break;
    case 15: // Store page fault
      printk("exception: store page fault at %p when executing %p\n", stval, sepc);
      break;
    default:
      printk("exception: scause = %ld, stval = %p, sepc = %p\n", scause, stval, sepc);
      break;
    }
    panic("exception occurred in kernel");
  } else {
    switch (scause) {
    case 2: // Invalid instruction
      printk("exception (user): pid %d (tid %d): invalid instruction %p when executing %p\n", pcb->pid, tcb->tid, stval, sepc);
      os::terminate(active(), -127, false);
      break;
    case 5:
      printk("exception (user): pid %d (tid %d): load access fault at %p when executing %p\n", pcb->pid, tcb->tid, stval, sepc);
      printk("page table flags: %x, physical address: %p\n", pte_flags(stval), to_pa(stval));
      os::terminate(active(), -127, false);
      break;
    case 8: { // System call
      auto trap = (trapframe *) tcb->ksp;
      tcb->a0 = trap->regs[8];
      tcb->sysret = true;
      trap->regs[8] = syshandle(trap); // a0
#ifndef NO_SYSCALL_LOG
      if (!ignored(trap->regs[15]))
        LOG_METHOD(" -> (%p)\n", trap->regs[8]);
#endif
      break;
    }
    case 12: // Instruction page fault
    case 13: // Load page fault
    case 15: // Store page fault.
      if (!vma::map_current((void *) stval))
        tcb->send_signal(SIGSEGV);
      break;
    default:
      printk("exception (user): scause = %ld, stval = %p, sepc = %p\n", scause & 0xff, stval, sepc);
      os::terminate(active(), -127, false);
    }
  }
}

}
