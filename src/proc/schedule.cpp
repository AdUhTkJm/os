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
  dispatch_impl();
}

void scheduler_t::dispatch_impl() {
  if (!ready.size())
    panic("scheduler: no ready process in queue!");
  auto next = ready.begin();
  ready.pop_front();
  printk("dispatched pid %d\n", next->pid);
  trap_return_setup(next);
}

void scheduler_t::erase(pcb_t *pcb) {
  synchronized syn(*lock);
  if (active == pcb) {
    // Now pcb is neither in ready nor in sleep.
    dispatch_impl();
    pcb->status = Zombie;
    return;
  }
  if (pcb->status == Ready)
    ready.erase(pcb);
  if (pcb->status == Sleeping)
    sleep.erase(pcb);
  pcb->status = Zombie;
}

void scheduler_t::yield(bool sleepy) {
  synchronized syn(*lock);
  (sleepy ? sleep : ready).push_back(active);
  active->status = sleepy ? Sleeping : Ready;
  dispatch_impl();
}

void scheduler_t::wakeup(pcb_t *pcb) {
  pcb->status = Ready;
  sleep.erase(pcb);
  ready.push_back(pcb);
}

}
