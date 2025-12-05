#ifndef SYSRET_H
#define SYSRET_H

// Note these are not in namespace os.
// These are returned structures from system call.
//
// These are typically directly taken from linux headers.

#define AT_FDCWD -100

// See <dirent.h>
struct linux_dirent64 {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  unsigned char type;
  char name[];
};

// See <linux/futex.h>
struct robust_list {
	struct robust_list *next;
};

struct robust_list_head {
	struct robust_list list;
	long futex_offset;
	struct robust_list *list_op_pending;
};

#endif
