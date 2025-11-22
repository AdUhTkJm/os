#include "utility.h"
#include "../../mem/kalloc.h"

void *operator new(size_t len) {
  return os::vmalloc(len);
}

void operator delete(void *ptr, size_t) {
  os::vfree(ptr);
}

void *operator new[](size_t len) {
  return os::vmalloc(len);
}

void operator delete[](void *ptr) {
  os::vfree(ptr);
}
