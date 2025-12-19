#ifndef MUTEX_H
#define MUTEX_H

#include "lock.h"
#include "../proc/pcb.h"

namespace os {

class mutex {
  spinlock lock;
  tcb_t *owner;
  os::list<tcb_t*> wait;

  friend class condvar;
public:
  void acquire();
  void release();
};

class condvar {
  spinlock spin;
  os::list<tcb_t*> queue;
  bool interrupt;
public:
  bool interrupted() { return interrupt; }
  
  // Atomically enqueues the thread, and releases the lock.
  void wait(mutex &lock);
  void notify();
  void notifyAll();
};

}

#endif
