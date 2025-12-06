#include "mutex.h"
#include "../proc/schedule.h"

namespace os {

void mutex::acquire() {
  lock.acquire();
  if (pid == -1) {
    pid = active()->pcb->pid;
    lock.release();
    return;
  }
  lock.release();
  scheduler.yield();
}

void mutex::release() {
  lock.acquire();
  if (!wait.empty()) {
    scheduler.wakeup(wait.back());
    wait.pop_back();
  }
  pid = -1;
  lock.release();
}

void condvar::wait() {
  spin.acquire();
  queue.push_back(active());
  spin.release();

  // sie bit 1: disables all interrupt.
  CSRC(sie, 2);
  lock.release();
  scheduler.yield();
  CSRS(sie, 2);
  
  lock.acquire();
}

void condvar::notify() {
  spin.acquire();
  if (!queue.empty()) {
    scheduler.wakeup(queue.back());
    queue.pop_back();
  }
  spin.release();
}

void condvar::notifyAll() {
  spin.acquire();
  while (!queue.empty()) {
    scheduler.wakeup(queue.back());
    queue.pop_back();
  }
  spin.release();
}

}
