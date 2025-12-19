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

  interrupt = (suspend(lock) != 0);
  lock.acquire();
}

void condvar::notify() {
  spin.acquire();
  if (!queue.empty()) {
    auto next = queue.front();
    queue.pop_front();
    scheduler.wakeup(next, /*can_preempt=*/ false);
  }
  spin.release();
  scheduler.maybe_preempt();
}

void condvar::notifyAll() {
  spin.acquire();
  while (!queue.empty()) {
    auto next = queue.front();
    queue.pop_front();
    scheduler.wakeup(next, /*can_preempt=*/ false);
  }
  spin.release();
  scheduler.maybe_preempt();
}

}
