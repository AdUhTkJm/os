#include "schedule.h"

namespace os {

scheduler_t scheduler;

void scheduler_t::add(pcb_t *pcb) {
  assert(pcb->status == Ready);
  ready.push_back(pcb);
}

// Implement better scheduling later. This is round-robin.
pcb_t *scheduler_t::choose() {
  if (!ready.size())
    panic("no ready process in queue!");
  if (active)
    ready.push_back(active);
  active = ready.begin();
  ready.pop_front();
  return active;
}

void scheduler_t::erase(pcb_t *pcb) {
  // This is not recorded in queues, so nothing needs to be done.
  // The callsite will handle it properly; this is not the concern
  // of a scheduler.
  if (active == pcb)
    return;
  if (pcb->status == Ready)
    ready.erase(pcb);
  if (pcb->status == Sleeping)
    sleep.erase(pcb);
}

}
