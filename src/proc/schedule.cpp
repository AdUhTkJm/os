#include "schedule.h"

namespace os {

scheduler_t scheduler;
static_storage<pcb_t> boot_pcb;

void scheduler_t::add(pcb_t *pcb) {
  synchronized syn(*lock);
  ready.push_back(pcb);
}

void scheduler_t::dispatch() {
  synchronized syn(*lock);
  dispatch_impl();
}

void scheduler_t::dispatch_impl() {
  assert(ready.size() != 0);
  // We first choose the next process.
  // Implement better scheduling later. This is round-robin.
  auto next = ready.begin();
  // This is the boot-time PCB. When scheduler is activated,
  // it's now useless.
  [[unlikely]] if (next->pid == -1) {
    ready.pop_front();
    next = ready.begin();
  }
  // This is the idle PCB. Don't dispatch it if we have something else.
  if (next->pid == 0 && ready.size() > 1) {
    ready.pop_front();
    auto n = ready.begin();
    // We can't push `next` back when it's in list.
    ready.push_back(next);
    next = n;
  }
  ready.pop_front();

  active = next;
  printk("dispatched pid %d\n", next->pid);
  trap_return_setup(next);
  
  // Now we switch to it.
  // The destructor is never run, so we manually unlock it.
  if (next->ctx_valid) {
    next->ctx_valid = false;
    lock->release();
    context_restore(&next->ctx); // noreturn
  }

  // If there's no ongoing syscall, then just directly jump to end.
  lock->release();
  __asm__ volatile("j __handler_end");
  __builtin_unreachable();
}

void scheduler_t::erase(pcb_t *pcb) {
  synchronized syn(*lock);
  if (active == pcb) {
    // Now pcb is neither in ready nor in sleep.
    pcb->status = Zombie;
    dispatch_impl();
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
  synchronized syn(*lock);
  pcb->status = Ready;
  sleep.erase(pcb);
  ready.push_back(pcb);
  // Preempt the idle pcb immediately. Don't wait till timer fire.
  if (active->pid == 0) {
    ready.push_back(active);
    active->status = Ready;
    dispatch_impl();
  }
}

}
