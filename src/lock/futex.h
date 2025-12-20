#ifndef FUTEX_H
#define FUTEX_H

#include "../proc/pcb.h"

namespace os {

struct futex_key {
  union {
    struct {
      vma::addrspace *mm;
      va_t addr;
    } priv;

    struct {
      inode *node;
      size_t offset;
    } shared;
  };
  enum : char {
    PRIVATE, SHARED, BAD
  } type;

  futex_key() = default;
  futex_key(va_t addr);
  bool operator==(const futex_key &other) const;
};

struct futex_queue {
  mutex lock;
  condvar wait;
};

// No need to define some special hash function.
static_assert(bitwise_hashable<futex_key>);
// futex_queue should not be copyable.
extern static_storage<os::hashmap<futex_key, futex_queue*>> futexes;

}

#endif
