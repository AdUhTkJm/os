#include "futex.h"
#include "../proc/schedule.h"

namespace os {

static_storage<os::hashmap<futex_key, futex_queue*>> futexes;

void futex_wait_queue::prepare(futex_wait_entry &entry) {
  synchronized _(lock);
  entry.tcb = active();
  if (!entry.queued) {
    entry.queued = true;
    q.push_back(&entry);
    scheduler.prepare_to_sleep();
  }
}

void futex_wait_queue::finish(futex_wait_entry &entry) {
  synchronized _(lock);
  if (entry.queued) {
    q.erase(&entry);
    entry.queued = false;
  }
}

int futex_wait_queue::wake(int n, unsigned mask) {
  lock.acquire();
  int woken = 0;
  for (auto it = q.begin(); it != q.end(); ++it) {
    auto entry = *it;
    assert(entry->queued);
    if (entry->tcb->status != Sleeping)
      continue;
    if ((entry->mask & mask) == 0)
      continue;
    
    scheduler.wakeup(entry->tcb, /*can_preempt=*/ false);
    woken++;
    if (!--n)
      break;
  }
  lock.release();
  return woken;
}

futex_key::futex_key(va_t addr) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto vmap = pcb->vma->find(addr);
  if (!vmap) {
    type = BAD;
    return;
  }

  const auto &vma = *vmap;
  if (vma.flags & MAP_SHARED) {
    type = SHARED;
    shared.node = vma.backup->node();
    shared.offset = vma.offset;
  } else {
    type = PRIVATE;
    priv.addr = addr;
    priv.mm = pcb->vma;
  }
}

}
