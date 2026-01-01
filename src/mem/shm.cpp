#include "shm.h"

namespace os::shm {

static_storage<os::hashmap<int, shared_memory>> shm;

void init() {
  shm.construct();
}

}
