#include "mutex.h"
#include "../proc/schedule.h"

namespace os {

void mutex::acquire() {
  auto tcb = active();

  lock.acquire();
  wait_entry entry;
  while (owner) {
    wait.prepare(entry);
    lock.release();
    suspend();
    lock.acquire();
    wait.finish(entry);
  }
  owner = tcb;
  lock.release();
}

void mutex::release() {
  lock.acquire();
  assert(owner == active());
  owner = nullptr;
  wait.wake();
    
  lock.release();
  // If the woken up thread has higher priority, let it run first.
  scheduler.maybe_preempt();
}

void condvar::wait(mutex &lock) {
  wait_entry entry;
  queue.prepare(entry);

  // Note that suspend() automatically re-acquires the lock on return.
  lock.release();
  suspend();
  lock.acquire();

  queue.finish(entry);
}

int condvar::notify(int max) {
  int woken = queue.wake(max);
  scheduler.maybe_preempt();
  return woken;
}

int condvar::notifyAll() {
  int woken = queue.wake_all();
  scheduler.maybe_preempt();
  return woken;
}

}
