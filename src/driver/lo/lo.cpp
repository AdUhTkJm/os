#include "lo.h"

namespace os::lo {

lo localhost;

int lo::write(const void *buf, int len, bool block) {
  (void) block; // This should never block.

  demux->push((const char*) buf, len);
  return len;
}

}
