#ifndef SHM_H
#define SHM_H

#include "../utils/helper.h"
#include "../fs/vfs.h"
#include "../interrupt/sysret.h"

namespace os::shm {

struct shared_memory {
  file *backup;
  struct meta {
    int attach = 0;
    bool removed = false;
    size_t atime, dtime, ctime;
  } meta;
};

extern static_storage<os::hashmap<int, shared_memory>> shm;

void init();

}

#endif
