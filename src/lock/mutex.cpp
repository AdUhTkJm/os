#include "mutex.h"
#include "../proc/schedule.h"

namespace os {

void mutex::acquire() {
  auto tcb = active();

  lock.acquire();
  while (owner) {
    wait.push_back(tcb);
    suspend(lock);
  }
  owner = tcb;
  lock.release();
}

void mutex::release() {
  lock.acquire();
  assert(owner == active());
  owner = nullptr;
  if (!wait.empty()) {
    auto next = wait.front();
    wait.pop_front();
    scheduler.wakeup(owner, /*can_preempt=*/ false);
  }
    
  lock.release();
  // If the woken up thread has higher priority, let it run first.
  scheduler.maybe_preempt();
}

void condvar::wait(mutex &lock) {
  spin.acquire();
  queue.push_back(active());
  spin.release();

  // Note that suspend() automatically re-acquires the lock on return.
  interrupt = (suspend(lock) != 0);
}

int condvar::notify(int max) {
  spin.acquire();
  int woken = 0;
  for (int i = 0; !queue.empty() && i < max; i++) {
    auto next = queue.front();
    queue.pop_front();
    scheduler.wakeup(next, /*can_preempt=*/ false);
    woken++;
  }
  spin.release();
  scheduler.maybe_preempt();
  return woken;
}

int condvar::notifyAll() {
  spin.acquire();
  int woken = 0;
  while (!queue.empty()) {
    auto next = queue.front();
    queue.pop_front();
    scheduler.wakeup(next, /*can_preempt=*/ false);
    woken++;
  }
  spin.release();
  scheduler.maybe_preempt();
  return woken;
}

}
