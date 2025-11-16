#include "utility.h"

void *operator new(size_t len) {
  return os::vmalloc(len);
}

void operator delete(void *ptr, size_t) {
  os::vfree(ptr);
}
