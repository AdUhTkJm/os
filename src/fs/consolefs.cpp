#include "consolefs.h"
#include "../utils/plic.h"

namespace os {

size_t stdin_inode::read(size_t offset, void *buf, size_t len) {
  (void) offset;
  char *p = (char *) buf;
  for (unsigned i = 0; i < len; i++)
    p[i] = console_input_buf->pop_front<block>();
  
  return len;
}

}