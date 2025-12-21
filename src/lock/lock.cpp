#include "lock.h"
#include "../proc/schedule.h"

namespace os {

void wait_queue::prepare(wait_entry &entry) {
  synchronized _(lock);
  entry.tcb = active();
  if (!entry.queued) {
    entry.queued = true;
    q.push_back(&entry);
    scheduler.prepare_to_sleep();
  }
}

void wait_queue::finish(wait_entry &entry) {
  synchronized _(lock);
  if (entry.queued) {
    q.erase(&entry);
    entry.queued = false;
  }
}

int wait_queue::wake_all() {
  lock.acquire();
  int woken = 0;
  for (auto entry : q) {
    if (!entry->queued)
      continue;
    woken++, scheduler.wakeup(entry->tcb, /*can_preempt=*/ false);
  }
  lock.release();

  scheduler.maybe_preempt();
  return woken;
}

int wait_queue::wake(int n) {
  lock.acquire();
  int woken = 0;
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (!(*it)->queued)
      continue;
    scheduler.wakeup((*it)->tcb, /*can_preempt=*/ false);
    woken++;
    if (!--n)
      break;
  }
  lock.release();
  
  scheduler.maybe_preempt();
  return woken;
}

}
