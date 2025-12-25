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
extern int timer_tick;
// In nanosecond, from Unix epoch. This is initially boot time.
extern size_t realtime;
// Default to zero (UTC).
timezone zone;
// Total amount of (virtually) shared memory.
size_t pshared;

// Returns current timestamp.
size_t os::now() {
  return rdtime() * timer_tick + realtime;
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

#define PRINT_FORMAT(x) #x " (%d): "
#define LOG_METHOD printk
#define PRINT1(x, a) LOG_METHOD(PRINT_FORMAT(x) #a " = %p", syscall::x, a0);
#define PRINT2(x, a, b) LOG_METHOD(PRINT_FORMAT(x) #a " = %p, " #b " = %p", syscall::x, a0, a1);
#define PRINT3(x, a, b, c) LOG_METHOD(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p", syscall::x, a0, a1, a2);
#define PRINT4(x, a, b, c, d) LOG_METHOD(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p", syscall::x, a0, a1, a2, a3);
#define PRINT5(x, a, b, c, d, e) LOG_METHOD(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p", syscall::x, a0, a1, a2, a3, a4);
#define PRINT6(x, a, b, c, d, e, f) LOG_METHOD(PRINT_FORMAT(x) #a " = %p, " #b " = %p, " #c " = %p, " #d " = %p, " #e " = %p, " #f " = %p", syscall::x, a0, a1, a2, a3, a4, a5);

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

HANDLE(read, fd, _buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  char *buf = new char[len];
  auto ret = file->read(buf, len);
  copy_to_user((void *) _buf, buf, len);
  return ret;
}

HANDLE(readv, fd, iov, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  if (cnt <= 0)
    return -EINVAL;

  auto iovecs = copy_from_user((void*) iov, cnt * sizeof(iovec));
  if (!iovecs)
    return -EFAULT;

  auto iovk = (iovec *) iovecs->get();
  long total = 0;
  for (int i = 0; i < cnt; i++) {
    const auto &v = iovk[i];

    if (v.iov_len == 0)
      continue;

    // Copy a single buffer.
    unique_ptr<char[]> buf(new char[v.iov_len]);
    ssize_t n = file->read(buf.get(), v.iov_len);
    if (n <= 0)
      return total ? total : n;

    copy_to_user(v.iov_base, buf.get(), v.iov_len);

    total += n;
    // If we have a partial read, then we stop immediately.
    if ((size_t) n < v.iov_len)
      break;
  }

  return total;
}

HANDLE(write, fd, _buf, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  auto buf = copy_from_user((void*) _buf, len);
  if (!buf)
    return -EFAULT;
  return file->write(buf->get(), len);
}

HANDLE(writev, fd, iov, cnt) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  if (cnt <= 0)
    return -EINVAL;

  auto iovecs = copy_from_user((void*) iov, cnt * sizeof(iovec));
  if (!iovecs)
    return -EFAULT;

  auto iovk = (iovec *) iovecs->get();
  long total = 0;
  for (int i = 0; i < cnt; i++) {
    const auto &v = iovk[i];

    if (v.iov_len == 0)
      continue;

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

HANDLE(truncate, _path, len) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return path;

  int fd = pcb->open_file(path->get(), O_WRONLY);
  if (fd < 0)
    return fd;

  auto file = pcb->ftbl->at(fd);
  return file->node()->truncate(len);
}

HANDLE(ftruncate, fd, len) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  return file->node()->truncate(len);
}

HANDLE(mkdirat, dirfd, _path, mode) {
  auto pathp = copy_from_user((char *) _path);
  if (!pathp)
    return -EFAULT;

  const char *path = pathp->get();
  bool relative = path[0] != '/';
  int flags = O_PATH | O_CREAT | O_EXCL;
  int fd = relative
    ? pcb->open_file_from(path, dirfd, flags, mode & ~pcb->umask, inode::Dir)
    : pcb->open_file(path, flags, mode & ~pcb->umask, inode::Dir);
  return fd < 0 ? fd : 0;
}

HANDLE(mknodat, dirfd, _path, mode, dev) {
  auto pathp = copy_from_user((char *) _path);
  if (!pathp)
    return -EFAULT;

  const char *path = pathp->get();
  bool relative = path[0] != '/';
  if (pcb->vfs->lookup(path))
    return -EEXIST;

  auto dir = dirname(path);
  int fd = relative
    ? pcb->open_file_from(dir, dirfd, O_RDONLY)
    : pcb->open_file(dir, O_RDONLY);
  
  auto file = pcb->ftbl->at(fd);
  auto inodety = ext_inode::totype((ext_inode::ftypeflags) (mode & ~0777));
  if (inodety == inode::Bad)
    return -EINVAL;
  if (inodety == inode::BlockDevice || inodety == inode::CharDevice) {
    printk("mknodat: unsupported: dev: %d\n", dev);
    return -EINVAL;
  }
  return file->node()->create(basename(path), inodety, mode & ~pcb->umask);
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
    auto p = copy_from_user((void *) offptr, sizeof(size_t));
    if (!p)
      return p;
    offset = *(size_t*) p->get();
  }

  unique_ptr<char[]> buf(new char[len]);
  int before = fin->seek(offset, file::begin);

  long read = fin->read(buf.get(), len);
  size_t written = fout->write(buf.get(), min(len, read));
  
  // We don't modify file position when offset is specified.
  if (offptr)
    fin->seek(before, file::begin);
  return written;
}

HANDLE(openat, dirfd, _path, flags, mode) {
  auto path = copy_from_user((char *) _path);
  if (!path || !*path)
    return -EFAULT;

  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, flags)
    : pcb->open_file(path->get(), flags);
  return fd;
}

HANDLE(close, fd) {
  return pcb->close_file(fd);
}

HANDLE(faccessat, dirfd, _path, mode) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  return detail::faccessat(dirfd, path->get(), mode);
}

HANDLE(utimensat, dirfd, _path, times, flags) {
  bool follow = !(flags & AT_SYMLINK_NOFOLLOW);
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  
  bool relative = (*path)[0] != '/';
  bool emptypath = flags & AT_EMPTY_PATH;
  file *file = nullptr;
  inode *node;
  if (!emptypath) {
    int fd = relative
      ? pcb->open_file_from(path->get(), dirfd, O_PATH)
      : pcb->open_file(path->get(), O_PATH);
    printk("fd = %d\n", fd);
    if (fd < 0)
      return fd;

    file = pcb->ftbl->at(fd);
    if (!file)
      return -EBADF;
    node = file->node();
  } else if (dirfd != AT_FDCWD) {
    file = pcb->ftbl->at(dirfd);
    if (!file)
      return -EBADF;
    node = file->node();
  } else node = pcb->pwd->node;

  if (!writable(pcb->uid, pcb->gid, node))
    return -EACCES;

  auto now = os::now();
  size_t atime = now, mtime = now;
  auto meta_before = node->get_meta();
  bool changed = false;
  if (times != 0) {
    auto timep = copy_from_user((void*) times, sizeof(timespec) * 2);
    if (!timep)
      return -EFAULT;
    auto time = (timespec *) timep->get();
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

  if (changed && node->uid != pcb->uid)
    return -EACCES;

  inode::meta meta(atime, now, mtime);
  node->set_meta(meta);
  return 0;
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
    return -EFAULT;
  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, O_RDONLY | O_NOFOLLOW)
    : pcb->open_file(path->get(), O_RDONLY | O_NOFOLLOW);
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

// These two system calls only exist in RISC-V.
#ifdef RV
HANDLE(fstatat, dirfd, _path, buf, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;
  int openflags = 0;
  if (flags & AT_SYMLINK_NOFOLLOW)
    openflags |= O_NOFOLLOW;
  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, O_PATH | openflags)
    : pcb->open_file(path->get(), O_PATH | openflags);
  if (fd < 0)
    return fd;
  auto node = pcb->ftbl->at(fd)->node();
  auto meta = node->get_meta();
  stat stat {
    .st_dev = 0,
    .st_ino = (unsigned long) node->inum(),
    .st_mode = (unsigned) (node->mode | ext_inode::fromtype(node->type)),
    .st_nlink = node->nlink(),
    .st_uid = (unsigned) node->uid,
    .st_gid = (unsigned) node->gid,
    .st_rdev = 0,
    .st_size = (long) node->size(),
    .st_blksize = 4096,
    .st_blocks = (long) (511 + node->size()) / 512,
    .st_atim = { .tv_sec = long(meta.atime / 1_s), .tv_nsec = long(meta.atime % 1_s) },
    .st_mtim = { .tv_sec = long(meta.mtime / 1_s), .tv_nsec = long(meta.mtime % 1_s) },
    .st_ctim = { .tv_sec = long(meta.ctime / 1_s), .tv_nsec = long(meta.ctime % 1_s) },
  };
  pcb->close_file(fd);
  copy_to_user((void *) buf, &stat, sizeof(stat));
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
    .st_size = (long) node->size(),
    .st_blksize = 1024,
    .st_blocks = 0,
    .st_atim = { .tv_sec = long(meta.atime / 1_s), .tv_nsec = long(meta.atime % 1_s) },
    .st_mtim = { .tv_sec = long(meta.mtime / 1_s), .tv_nsec = long(meta.mtime % 1_s) },
    .st_ctim = { .tv_sec = long(meta.ctime / 1_s), .tv_nsec = long(meta.ctime % 1_s) },
  };
  copy_to_user((void *) buf, &stat, sizeof(stat));
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

  file->node()->fs->sync();
  return 0;
}

HANDLE(fdatasync, fd) {
  // TODO: We still flush the entire file system.
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  file->node()->fs->sync();
  return 0;
}

HANDLE(unlinkat, dirfd, _path, flags) {
  auto path = copy_from_user((char *) _path);
  if (!path)
    return -EFAULT;

  bool relative = (*path)[0] != '/';
  int fd = relative
    ? pcb->open_file_from(path->get(), dirfd, O_PATH)
    : pcb->open_file(path->get(), O_PATH);
  if (fd < 0)
    return fd;

  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  // We cannot unlink a directory.
  if (file->node()->type == inode::Dir)
    return -EISDIR;

  auto dir = file->entry->parent->node;
  if (!writable(pcb->uid, pcb->gid, dir))
    return -EACCES;

  // unlink() will check whether `dir` is indeed a dir.
  int ret = dir->unlink(file->entry->name);
  if (ret < 0)
    return ret;

  // Forget it in dcache.
  vfs::invalidate(file->entry->parent->node, file->entry->name);
  return 0;
}

HANDLE(brk, addr) {
  return pcb->brk(addr);
}

HANDLE(dup, fd) {
  auto file = pcb->ftbl->at(fd);
  if (!file)
    return -EBADF;

  // Increase pipe reader/writer counts.
  if (auto node = dyn_cast<pipe_inode>(file->node()))
    node->incf(file);
  
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

  if (auto node = dyn_cast<pipe_inode>(file->node()))
    node->incf(file);

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
  return pcb->uid;
}

HANDLE(geteuid, _) {
  return pcb->euid;
}

HANDLE(getegid, _) {
  return pcb->gid;
}

HANDLE(getgid, _) {
  return pcb->egid;
}

HANDLE(gettid, _) {
  return tcb->tid;
}

HANDLE(set_tid_address, _) {
  return -ENOSYS; // TODO
}

HANDLE(getrandom, _) {
  return -ENOSYS; // TODO
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
  copy_to_user((void*) buf, path.c_str(), path.size() + 1);
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
  copy_to_user((void *) buf, &name, sizeof(utsname));
  return 0;
}

HANDLE(get_robust_list, pid, headptr, size) {
  return -ENOSYS;
}

HANDLE(set_robust_list, headptr, size) {
  return -ENOSYS;
}

HANDLE(rseq, _) {
  return -ENOSYS;
}

#ifdef RV
HANDLE(riscv_hwprobe, _) {
  return -ENOSYS;
}
#endif

HANDLE(times, buf) {
  copy_to_user((void *) buf, &pcb->times, sizeof(tms));
  return timer_tick;
}

HANDLE(clone, flags, stack, parenttid, tls, childtid) {
  tcb_t *tcb = os::clone(flags, stack, (void *) tls);
  if (childtid)
    copy_to_user((void *) childtid, &tcb->tid, sizeof(int));
  return tcb->pcb->pid;
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
  panic("exit: unreachable");
}

HANDLE(exit_group, ret) {
  os::terminate(pcb, ret);
  panic("exit_group: unreachable");
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
  constexpr unsigned nameoff = offsetof(linux_dirent64, name);
  for (unsigned i = file->offset; i < items.size(); i++) {
    const auto &item = items[i];
    // Maintain alignment.
    unsigned short len = roundup<8>(nameoff + item.name.size() + 1);
    if (va_t(pos) - dirents + len >= va_t(cnt))
      break;

    unsigned char type = inode::as_dt(item.ty);
    linux_dirent64 entry { .inum = (unsigned long) item.inum, ._resv = 0, .len = len, .type = type };
    copy_to_user(pos, &entry, nameoff);
    copy_to_user(pos + nameoff, item.name.c_str(), item.name.size() + 1);
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

  auto ret = detail::mount(src->get(), tgt->get(), fsty->get(), flags);
  (void) data;
  return ret;
}

HANDLE(chroot, _path) {
  // Only root can chroot.
  if (pcb->euid != 0)
    return -EACCES;

  auto path = copy_from_user((char *) _path);
  if (!path || !*path)
    return -EFAULT;

  auto dentry = pcb->vfs->lookup(path->get());
  if (!dentry)
    return dentry;
  if ((*dentry)->node->type != inode::Dir)
    return -ENOTDIR;

  return pcb->vfs->chroot(*dentry);
}

HANDLE(prlimit64, pid, resource, new_rlim, old_rlim) {
  return detail::prlimit64(pid, resource, (void *) new_rlim, (void *) old_rlim);
}

HANDLE(ioctl, fd, op, argp) {
  return detail::ioctl(fd, op, (void *) argp);
}

HANDLE(clock_gettime, id, tp) {
  timespec spec;
  size_t time = rdtime() * timer_tick;

  switch (id) {
  case CLOCK_MONOTONIC:
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
  copy_to_user((void *) tp, &spec, sizeof(timespec));
  return 0;
}

HANDLE(gettimeofday, tv, tz) {
  if (tz)
    copy_to_user((void*) tz, &zone, sizeof(timezone));
  
  if (tv) {
    auto cur = now();
    timeval ts {
      .tv_sec = long(cur / 1_s),
      .tv_usec = long(cur % 1_s) / 1000,
    };
    copy_to_user((void*) tv, &ts, sizeof(timeval));
  }
  return 0;
}

HANDLE(settimeofday, tv, tz) {
  if (tz)
    // TODO: What is a valid timezone anyway?
    return -EINVAL;
  if (tv) {
    // Note this is a timeval!
    auto val = copy_from_user((void*) tv, sizeof(timeval));
    if (!val)
      return val;
    auto ts = (timeval*) val->get();
    unsigned long time = ts->tv_usec * 1_us + ts->tv_sec * 1_s;
    unsigned long tick = rdtime() * (unsigned long) timer_tick;
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
  bool shared = flags & MAP_SHARED;
  bool priv = flags & MAP_PRIVATE;
  if ((!shared && !priv) || len == 0)
    return -EINVAL;

  bool fixed = flags & MAP_FIXED;
  bool anon = flags & MAP_ANONYMOUS;

  va_t start;
  if (!fixed) {
    start = 0x6000'0000;
    for (const auto &vma : pcb->vma) {
      if (vma.flags & VMA_IS_STACK)
        continue;
      start = max(start, vma.end);
    }
    start = rounddown<PAGE_SIZE>(start);
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

  // Now allocate near this cap. Note that this has to be page-aligned.
  vma::vma_t vma(start, end, prot, flags, backup, offset, len);
  pcb->vma.push(vma);
  return vma.begin;
}

HANDLE(mprotect, start, len, prot) {
  // Find the VMA that contains this mprotect.
  return detail::mprotect(start, len, prot);
}

HANDLE(munmap, addr, len) {
  return detail::munmap(addr, len);
}

HANDLE(sched_yield, _) {
  // There is no system call context when we directly call yield().
  scheduler.yield();
  // noreturn
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

  auto sigset = copy_from_user((void *) sig, sizeof(sigset_t));
  if (!sigset)
    return sigset;

  size_t tm = 0;
  if (timeout) {
    auto timep = copy_from_user((void *) timeout, sizeof(timespec));
    if (!timep)
      return timep;
    auto time = (timespec *) timep->get();
    tm = time->tv_nsec + time->tv_sec * 1_s;
  }

  auto wait = *(unsigned long*) sigset->get();
  // In Linux, the bit is usually `sig - 1`.
  wait <<= 1;
  if (tm == 0) {
    if (wait & tcb->pending.sig)
      return tcb->pending.next(~wait);
    return -EAGAIN;
  }

  tcb->sigresume = -1;
  tcb->sigwait = wait;
  tcb->sleep(tm);
  return tcb->sigresume != -1 ? tcb->sigresume : -EAGAIN;
}

HANDLE(kill, pid, sig) {
  if (pid == 0)
    pid = pcb->pid;
  
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

HANDLE(ppoll, _fds, cnt, tmo, sigmask) {
  // Ignore sigmask for now.
  if (cnt <= 0)
    return -EINVAL;
  auto pollfds = copy_from_user((void *) _fds, cnt * sizeof(pollfd));
  if (!pollfds)
    return pollfds;
  
  size_t timeout = -1ul;
  if (tmo) {
    auto tp = copy_from_user((void *) tmo, sizeof(timespec));
    if (!tp)
      return tp;
    timespec t = *(timespec *) tp->get();
    timeout = t.tv_nsec + t.tv_sec * 1_s;
  }
retry:
  auto fds = (pollfd*) pollfds->get();
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
    copy_to_user((void *) _fds, fds, cnt * sizeof(pollfd));
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
  // Recovered from sleeping by something other than interrupt.
  if (ret == 1)
    goto retry;
  // Recovered from sleeping by interrupt.
  return -EINTR;
}

HANDLE(nanosleep, rqtp, rmtp) {
  return detail::nanosleep(CLOCK_MONOTONIC, 0, (void *) rqtp, (void *) rmtp);
}

HANDLE(clock_nanosleep, clock, flags, rqtp, rmtp) {
  return detail::nanosleep(clock, flags, (void *) rqtp, (void *) rmtp);
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
    pcb->sigact[sig] = a;
  }
  return 0;
}

HANDLE(futex, uaddr, op, val, timeout) {
  return detail::futex((void *) uaddr, op, val, (void *) timeout);
}

HANDLE(wait4, pid, wstatus, options, rusage) {
  return detail::wait(pid, (void *) wstatus, options, (void *) rusage);
}

HANDLE(reboot, magic, magic2, op, arg) {
  if (magic != 0xfee1dead)
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

  auto pipe = pipefs.get(); // On creation it has a writer and a reader, so don't increment.

  file *read = new file(new dentry("<pipe r>", pipe, nullptr), O_RDONLY | extra);
  file *write = new file(new dentry("<pipe w>", pipe, nullptr), O_WRONLY | extra);

  int fd[2] = { pcb->ftbl->allocate(read), pcb->ftbl->allocate(write) };
  copy_to_user((void*) fds, fd, sizeof(fd));
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

HANDLE(sendto, fd, buf, size, flags, dest, addrlen) {
  return detail::sendto(fd, (void *) buf, size, flags, (void *) dest, addrlen);
}

HANDLE(sendmsg, fd, msg, flags) {
  return detail::sendmsg(fd, (void *) msg, flags);
}

HANDLE(sendmmsg, fd, msg, n, flags) {
  auto mp = copy_from_user((void *) msg, sizeof(mmsghdr) * n);
  if (!mp)
    return mp;
  auto messages = (mmsghdr *) mp->get();

  int i = 0;
  for (; i < n; i++) {
    int sent = detail::sendmsg(fd, messages[i].msg_hdr, flags);
    if (sent < 0)
      return i ? i : sent;
    
    copy_to_user((void*) (msg + sizeof(mmsghdr) * i + offsetof(mmsghdr, msg_len)), &sent, sizeof(unsigned));
  }
  return i;
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
  copy_to_user((void *) info, &sysinfo, sizeof(struct sysinfo));
  return 0;
}

HANDLE(setitimer, which, timer, old) {
  if (which < 0 || which >= 3)
    return -EINVAL;
  if (which != ITIMER_REAL) {
    printk("setitimer: no timer %d yet\n", timer);
    return -EINVAL;
  }
  
  auto p = copy_from_user((void *) timer, sizeof(itimerval));
  if (!p)
    return p;
  itimerval time = *(itimerval *) p->get();
  if (time.it_interval.tv_usec > 999999 || time.it_interval.tv_usec < 0)
    return -EINVAL;
  if (time.it_value.tv_usec > 999999 || time.it_value.tv_usec < 0)
    return -EINVAL;
  
  auto intv = time.it_interval.tv_usec * 1000 + time.it_interval.tv_sec * 1_s;
  auto tm = time.it_value.tv_usec * 1000 + time.it_value.tv_sec * 1_s;

  pcb->itimers[which].interval = (intv + tick_length - 1) / tick_length;
  pcb->itimers[which].timeout = (tm + tick_length - 1) / tick_length;
  scheduler.record_itimer_real(pcb);

  if (old) {
    auto timer = pcb->itimers[which];
    auto intv = timer.interval * tick_length;
    auto time = timer.timeout * tick_length + now();
    itimerval v {
      .it_interval = { .tv_sec = long(intv / 1_s), .tv_usec = long(intv / 1_us) },
      .it_value    = { .tv_sec = long(time / 1_s), .tv_usec = long(time / 1_us) }
    };
    copy_to_user((void *) old, &v, sizeof(itimerval));
  }
  return 0;
}

HANDLE(getitimer, which, old) {
  if (which < 0 || which >= 3)
    return -EINVAL;

  auto timer = pcb->itimers[which];
  auto intv = timer.interval * tick_length;
  auto time = timer.timeout * tick_length + now();
  itimerval v {
    .it_interval = { .tv_sec = long(intv / 1_s), .tv_usec = long(intv / 1_us) },
    .it_value    = { .tv_sec = long(time / 1_s), .tv_usec = long(time / 1_us) }
  };
  copy_to_user((void *) old, &v, sizeof(timeval) * 2);
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
  if (!tcb->kmode)
    tcb->kmode = true;

  if (scause < 0) {
    int kind = scause & 0xff;
    switch (kind) {
    case 5: { // Timer interrupt
      // Tick every 100ms.
      sbi_set_timer(rdtime() + tick_length / timer_tick);
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
    auto pid = active()->pcb->pid;
    switch (scause) {
    case 2: // Invalid instruction
      printk("exception (user): pid %d: invalid instruction %p when executing %p\n", pid, stval, sepc);
      os::terminate(active(), -127);
      break;
    case 5:
      printk("exception (user): pid %d: load access fault at %p when executing %p\n", pid, stval, sepc);
      printk("page table flags: %x, physical address: %p\n", pte_flags(stval), to_pa(stval));
      os::terminate(active(), -127);
      break;
    case 8: { // System call
      auto pcb = active();
      auto trap = (trapframe *) pcb->ksp;
      trap->regs[8] = syshandle(trap); // a0
#ifndef NO_SYSCALL_LOG
      LOG_METHOD(" -> (%p)\n", trap->regs[8]);
#endif
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
