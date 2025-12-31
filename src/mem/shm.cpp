#include "shm.h"

namespace os::shm {

static_storage<os::hashmap<int, file*>> shm;

void init() {
  shm.construct();
}

}
