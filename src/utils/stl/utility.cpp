#include "utility.h"
#include "../../mem/kalloc.h"

void *operator new(size_t len) {
  return os::vmalloc(len);
}

void *operator new(size_t len, os::permanent_t) {
  return os::vmalloc(len, /*permanent=*/ true);
}

void operator delete(void *ptr, size_t) {
  os::vfree(ptr);
}

void operator delete(void *ptr) {
  os::vfree(ptr);
}

void *operator new[](size_t len) {
  return os::vmalloc(len);
}

void *operator new[](size_t len, os::permanent_t) {
  return os::vmalloc(len, /*permanent=*/ true);
}

void operator delete[](void *ptr) {
  os::vfree(ptr);
}

void operator delete[](void *ptr, size_t) {
  os::vfree(ptr);
}
