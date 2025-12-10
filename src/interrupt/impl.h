#ifndef IMPL_H
#define IMPL_H

// Complicated system call entries are separated into different files.
// Putting them in a single file can make the complication slower.

namespace os::detail {

int mount(const char *src, const char *tgt, const char *fsty, unsigned long flags);
int fcntl(int fd, int ty, int arg);
int mprotect(unsigned long start, unsigned long len, int prot);
int ioctl(int fd, int op, void *argp);

}

#endif
