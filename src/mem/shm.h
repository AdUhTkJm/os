#ifndef SHM_H
#define SHM_H

#include "../utils/helper.h"
#include "../fs/vfs.h"

namespace os::shm {

extern static_storage<os::hashmap<int, file*>> shm;

void init();

}

#endif
