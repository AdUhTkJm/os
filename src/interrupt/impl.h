#ifndef IMPL_H
#define IMPL_H

// Complicated system call entries are separated into different files.
// Putting them in a single file can make the complication slower.

#include "../fs/vfs.h"

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
int syslog(int type, char *buf, unsigned long size);

int fromtype(inode::filetype ty);

}

#endif
