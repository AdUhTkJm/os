#ifndef FUTEX_H
#define FUTEX_H

#include "../proc/pcb.h"

namespace os {

// WARNING: We CANNOT HAVE ANY PADDING in this structure.
// We're directly bitwise-hashing it.
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
  enum : unsigned long {
    PRIVATE, SHARED, BAD
  } type;

  futex_key() = default;
  futex_key(va_t addr);
  bool operator==(const futex_key &other) const { return memcmp(this, &other, sizeof(futex_key)) == 0; }
};
// This means `operator==` is semantically correct if implemented with memcmp.
static_assert(__has_unique_object_representations(futex_key));

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

// futex_queue should not be copyable.
extern static_storage<os::hashmap<futex_key, futex_queue*>> futexes;

}

#endif
