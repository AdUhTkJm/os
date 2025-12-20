#ifndef MUTEX_H
#define MUTEX_H

#include "lock.h"
#include "../utils/stl/list.h"

namespace os {

struct tcb_t;

class mutex {
  spinlock lock;
  tcb_t *owner = nullptr;
  os::list<tcb_t*> wait;

  friend class condvar;
public:
  void acquire();
  void release();
};

class condvar {
  spinlock spin;
  os::list<tcb_t*> queue;
  bool interrupt = false;
public:
  bool interrupted() { return interrupt; }
  
  // Atomically enqueues the thread, and releases the lock.
  void wait(mutex &lock);
  void notify();
  void notifyAll();
};

}

#endif
