#ifndef IMPL_H
#define IMPL_H

// Complicated system call entries are separated into different files.
// Putting them in a single file can make the complication slower.

#include "../fs/vfs.h"

struct msghdr;

namespace os::detail {

int mount(const char *src, const char *tgt, const char *fsty, unsigned long flags);
int fcntl(int fd, int ty, int arg);
int mprotect(unsigned long start, unsigned long len, int prot);
int munmap(unsigned long addr, unsigned long len);
int ioctl(int fd, int op, void *argp);
int wait(int pid, void *wstatus, int options, void *rusage);
int faccessat(int dirfd, const char *path, int mode);
int socket(int domain, int type, int protocol);
int bind(int fd, void *sockaddr, unsigned len);
int connect(int fd, void *sockaddr, unsigned len);
int syslog(int type, char *buf, unsigned long size);
int futex(void *uaddr, int op, int val, void *timeout);
int setsockopt(int fd, int level, int optname, void *optval, int optlen);
int getsockopt(int fd, int level, int optname, void *optval, int *optlen);
int sendto(int fd, void *buf, unsigned long size, int flags, void *dest, unsigned addrlen);
int sendmsg(int fd, void *msg, int flags);
int sendmsg(int fd, const msghdr &msg, int flags);
int prlimit64(int pid, int resource, void *newrlim, void *oldrlim);
int nanosleep(int clock, int flags, void *rqtp, void *rmtp);

int fromtype(inode::filetype ty);

}

#endif
