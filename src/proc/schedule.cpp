#include "schedule.h"

namespace os {

scheduler_t scheduler;
static_storage<tcb_t> boot_tcb;
static_storage<pcb_t> boot_pcb;
static_storage<os::list<tcb_t*>> napping;

void scheduler_t::add(tcb_t *tcb) {
  synchronized syn(lock);
  ready.push_back(tcb);
}

void scheduler_t::dispatch() {
  synchronized syn(lock);
  dispatch_impl();
}

void scheduler_t::dispatch_impl() {
  assert(ready.size() != 0);
  // We first choose the next process.
  // Implement better scheduling later. This is round-robin.
  auto next = ready.front();
  // This is the boot-time PCB. When scheduler is activated,
  // it's now useless.
  [[unlikely]] if (next->pcb->pid == -1) {
    ready.pop_front();
    next = ready.front();
  }
  // This is the idle PCB. Don't dispatch it if we have something else.
  if (next->pcb->pid == 0 && ready.size() > 1) {
    ready.pop_front();
    auto n = ready.front();
    // We can't push `next` back when it's in list.
    ready.push_back(next);
    next = n;
  }
  ready.pop_front();

  active = next;
  trap_return_setup(next);
  
  // Now we switch to it.
  // The destructor is never run, so we manually unlock it.
  if (next->ctx_valid) {
    next->ctx_valid = false;
    lock.release();
    // `next` resumes because of a signal, if it has any pending signals.
    // (Masked signals won't even reach pending point.)
    context_restore(&next->ctx, /*from_signal=*/ next->pending.sig != 0); // noreturn
  }

  // If there's no ongoing syscall, then just directly jump to end.
  lock.release();
  __asm__ volatile("j __handler_end");
  __builtin_unreachable();
}

void scheduler_t::erase(tcb_t *pcb) {
  synchronized syn(lock);
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
  synchronized syn(lock);
  (sleepy ? sleep : ready).push_back(active);
  active->status = sleepy ? Sleeping : Ready;
  dispatch_impl();
}

void scheduler_t::maybe_preempt_impl() {
  // Preempt the idle pcb immediately. Don't wait till timer fire.
  if (active->pcb->pid == 0) {
    ready.push_back(active);
    active->status = Ready;
    dispatch_impl();
  } else
    lock.release();
}

void scheduler_t::wakeup(tcb_t *tcb, bool can_preempt) {
  lock.acquire();
  tcb->status = Ready;
  sleep.erase(tcb);
  ready.push_back(tcb);
  if (can_preempt)
    maybe_preempt_impl();
  else
    lock.release();
}

void scheduler_t::unnap(tcb_t *tcb, bool wake) {
  for (auto it = napping->begin(); it != napping->end(); ++it) {
    if (*it == tcb) {
      napping->erase(it);
      if (wake)
        wakeup(tcb, /*can_preempt=*/ false);
      break;
    }
  }
}

// TODO: unnecessary O(n^2).
void scheduler_t::tick() {
  os::vector<tcb_t*> tounnap;
  for (auto tcb : *napping) {
    if (--tcb->timeout <= 0)
      tounnap.push_back(tcb);
  }
  for (auto tcb : tounnap)
    unnap(tcb);
}

}
