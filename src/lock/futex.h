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

struct futex_wait_entry : intrusive_list_node<futex_wait_entry> {
  tcb_t *tcb;
  unsigned mask;
  bool queued = false;
};

struct futex_wait_queue {
  spinlock lock;
  os::intrusive_list<futex_wait_entry> q;

  void prepare(futex_wait_entry &entry);
  void finish(futex_wait_entry &entry);
  int wake(int n, unsigned mask);
};

struct futex_queue {
  spinlock lock;
  futex_wait_queue wait;

  futex_queue() = default;
  futex_queue(const futex_queue &other) = delete;
  futex_queue &operator=(const futex_queue &other) = delete;
};

// No need to define some special hash function.
static_assert(bitwise_hashable<futex_key>);
// futex_queue should not be copyable.
extern static_storage<os::hashmap<futex_key, futex_queue*>> futexes;

}

#endif
