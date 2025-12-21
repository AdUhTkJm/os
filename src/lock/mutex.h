#ifndef MUTEX_H
#define MUTEX_H

#include "lock.h"
#include "../utils/stl/list.h"

namespace os {

struct tcb_t;

class mutex {
  spinlock lock;
  tcb_t *owner = nullptr;
  wait_queue wait;

  friend class condvar;
public:
  mutex() = default;
  mutex(const mutex &other) = delete;
  mutex &operator=(const mutex &other) = delete;

  void acquire();
  void release();
};

// To keep things synchronized:
// the condition variable must be waited on using the same lock that protects the condition being tested.
class condvar {
  spinlock spin;
  wait_queue queue;
  bool interrupt = false;
public:
  condvar() = default;
  condvar(const condvar &other) = delete;
  condvar &operator=(const condvar &other) = delete;

  bool interrupted() { return interrupt; }
  
  // Atomically enqueues the thread, and releases the lock.
  void wait(mutex &lock);
  int notify(int max = 1);
  int notifyAll();
};

}

#endif
