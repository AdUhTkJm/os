#include "schedule.h"

namespace os {

scheduler_t scheduler;
static_storage<pcb_t> boot_pcb;

void scheduler_t::add(pcb_t *pcb) {
  synchronized syn(*lock);
  assert(pcb->status == Init);
  ready.push_back(pcb);
}

// Implement better scheduling later. This is round-robin.
void scheduler_t::dispatch() {
  synchronized syn(*lock);
  if (!ready.size())
    panic("scheduler: no ready process in queue!");
  auto next = ready.begin();
  ready.pop_front();
  trap_return_setup(next);
}

void scheduler_t::erase(pcb_t *pcb) {
  synchronized syn(*lock);
  pcb->status = Zombie;
  if (active == pcb) {
    dispatch();
    return;
  }
  if (pcb->status == Ready)
    ready.erase(pcb);
  if (pcb->status == Sleeping)
    sleep.erase(pcb);
}

void scheduler_t::yield(bool sleepy) {
  synchronized syn(*lock);
  (sleepy ? sleep : ready).push_back(active);
  active->status = sleepy ? Sleeping : Ready;
  dispatch();
}

void scheduler_t::wakeup(pcb_t *pcb) {
  pcb->status = Ready;
  sleep.erase(pcb);
  ready.push_back(pcb);
}

}
