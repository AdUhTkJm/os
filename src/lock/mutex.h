#ifndef MUTEX_H
#define MUTEX_H

#include "lock.h"
#include "../proc/pcb.h"

namespace os {

class mutex {
  spinlock lock;
  int pid = -1;            // The pid of current lock owner.
  os::vector<pcb_t*> wait; // All pids that wait on this lock.
public:
  void acquire();
  void release();
};

class condvar {
  mutex &lock;
  spinlock spin;
  os::vector<pcb_t*> queue;
public:
  condvar(mutex &lock): lock(lock) {}
  void wait();
  void notify();
  void notifyAll();
};

}

#endif
