#ifndef SYSRET_H
#define SYSRET_H

// Note these are not in namespace os.
// These are returned structures from system call.

struct linux_dirent {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  char name[];
  // the last byte should be file type.
};

struct linux_dirent64 {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  unsigned char type;
  char name[];
};

#endif
