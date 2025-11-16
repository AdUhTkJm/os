#ifndef VFS_H
#define VFS_H

#include "fd.h"

namespace os {

struct mount {
  inode *root;
  const char *fs_type;
};

extern os::static_storage<os::vector<mount>> mounts;

}

#endif
