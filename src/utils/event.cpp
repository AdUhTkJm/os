#include "event.h"

namespace os::event {

static_storage<os::vector<on_terminate>> terminate;

void init() {
  terminate.construct();
}

void record(on_terminate f) {
  terminate->push_back(f);
}

};
