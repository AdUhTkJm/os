#ifndef SYSRET_H
#define SYSRET_H

// Note these are not in namespace os.
// These are returned structures from system call.

#define AT_FDCWD -100

struct linux_dirent64 {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  unsigned char type;
  char name[];
};

#endif
