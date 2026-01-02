#include "shm.h"

namespace os::shm {

int nextid;
static_storage<os::hashmap<int, int>> key2id;
static_storage<os::hashmap<int, shared_memory>> shm;

void init() {
  key2id.construct();
  shm.construct();
}

}
