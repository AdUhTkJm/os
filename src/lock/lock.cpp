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
    assert(entry->queued);
    if (entry->tcb->status == Sleeping)
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
    auto entry = *it;
    assert(entry->queued);
    if (entry->tcb->status != Sleeping)
      continue;
    
    scheduler.wakeup(entry->tcb, /*can_preempt=*/ false);
    woken++;
    if (!--n)
      break;
  }
  lock.release();
  
  scheduler.maybe_preempt();
  return woken;
}

void wait_queue::wake(wait_entry &entry, bool can_preempt) {
  lock.acquire();
  assert(entry.queued);
  if (entry.tcb->status == Sleeping)
    scheduler.wakeup(entry.tcb, /*can_preempt=*/ false);
  lock.release();

  if (can_preempt)
    scheduler.maybe_preempt();
}

#ifdef DEADLOCK
// Called by context_save.
void check_suspend() {
  if (detail::nested_irq > 0)
    panic("check_suspend: deadlock");
}
#endif

}
