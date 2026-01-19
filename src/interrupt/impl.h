#ifndef IMPL_H
#define IMPL_H

// Complicated system call entries are separated into different files.
// Putting them in a single file can make the complication slower.

#include "../fs/vfs.h"

struct msghdr;

namespace os::detail {

long mount(const char *src, const char *tgt, const char *fsty, unsigned long flags);
long fcntl(int fd, int ty, unsigned long arg);
long mmap(unsigned long addr, unsigned long len, int prot, int flags, int fd, unsigned long offset);
long mprotect(unsigned long start, unsigned long len, int prot);
long munmap(unsigned long addr, unsigned long len);
long mremap(unsigned long addr, unsigned long len, unsigned long newlen, int flags, unsigned long newaddr);
long ioctl(int fd, int op, void *argp);
long wait(int pid, void *wstatus, int options, void *rusage);
long faccessat(int dirfd, const char *path, int mode, int flags);
long socket(int domain, int type, int protocol);
long bind(int fd, void *sockaddr, unsigned len);
long connect(int fd, void *sockaddr, unsigned len);
long syslog(int type, char *buf, unsigned long size);
long futex_wait(void *addr, int expected, void *_timeout, int tmtype, unsigned mask = -1);
long futex_wake(void *addr, int count, unsigned mask = -1);
long futex(void *uaddr, int op, int val, void *timeout, unsigned long val2, unsigned long val3);
long setsockopt(int fd, int level, int optname, void *optval, int optlen);
long getsockopt(int fd, int level, int optname, void *optval, int *optlen);
long getsockname(int fd, void *sockname, void *len);
long sendto(int fd, void *buf, unsigned long size, int flags, void *dest, unsigned addrlen);
long sendmsg(int fd, void *msg, int flags);
long sendmsg(int fd, const msghdr &msg, int flags);
long recvfrom(int fd, void *buf, unsigned long size, int flags, void *src, unsigned addrlen);
long recvmsg(int fd, void *msg, int flags);
long prlimit64(int pid, int resource, void *newrlim, void *oldrlim);
long nanosleep(int clock, int flags, void *rqtp, void *rmtp);
long clone(int flags, unsigned long stack, void *parenttid, void *tls, void *childtid);
long shmget(int key, unsigned long len, int flags);
long shmat(int key, unsigned long addr, int flags);
long shmdt(unsigned long addr);
long shmctl(int key, int op, void *buf);
long ppoll(void *_fds, unsigned cnt, unsigned long tmo, void *sigmask, bool isuser);
long rename(int olddirfd, unsigned long oldpath, int newdirfd, unsigned long newpath, int flags);

long fromtype(inode::filetype ty);
long read_to_user(file *f, void *buf, size_t len);
long write_from_user(file *f, void *buf, size_t len);

}

#endif
