#include "consolefs.h"
#include "../utils/plic.h"
#include "../proc/schedule.h"

namespace os {

static_storage<console_inode> tty0;

size_t console_inode::read(size_t offset, void *buf, size_t len) {
  (void) offset;
  char *p = (char *) buf;
  for (unsigned i = 0; i < len; i++) {
    auto c = console_input_buf->pop_front();
    if (!c)
      return i;
    p[i] = *c;
  }
  return len;
}

size_t console_inode::write(size_t offset, const void *buf, size_t len) {

}

void console_inode::wake() {
  synchronized syn(lock);
  scheduler.wakeup(wait.front());
  wait.pop_front();
}

}