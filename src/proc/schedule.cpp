#include "schedule.h"
#include "../fs/devfs.h"

namespace os {

scheduler_t scheduler;
static_storage<tcb_t> boot_tcb;
static_storage<pcb_t> boot_pcb;
static_storage<os::list<pcb_t*>> itimer_real;
wait_queue napping;

void scheduler_t::add(tcb_t *tcb) {
  synchronized _(lock);
  ready.push_back(tcb);
}

void scheduler_t::dispatch() {
  lock.acquire();
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

  // Update time information.
  size_t time = now();
  if (active->last_sched != 0) {
    long delta = time - active->last_sched;
    active->ruse.ru_nvcsw++;
    if (active->kmode)
      active->ruse.ru_stime += delta;
    else
      active->ruse.ru_utime += delta;
  }
  next->last_sched = time;

  active = next;
  trap_return_setup(next);
  
  // Now we switch to it.
  if (next->ctx_valid) {
    next->ctx_valid = false;
    lock.release();
    // `next` resumes because of a signal, if it has any pending signals.
    // (Masked signals won't even reach pending point.)
    context_restore(&next->ctx, /*from_signal=*/ next->pending.sig != 0); // noreturn
  }

  // If there's no ongoing syscall, then just directly jump to end,
  // and return from this interrupt.
  lock.release();
#ifdef RV
  __asm__ volatile("j __handler_end");
#endif
#ifdef LA
  __asm__ volatile("b __handler_end");
#endif
  __builtin_unreachable();
}

void scheduler_t::erase(tcb_t *tcb) {
  lock.acquire();
  if (tcb->status == Ready)
    ready.erase(tcb);
  if (tcb->status == Sleeping)
    sleep.erase(tcb);
  tcb->status = Zombie;
  if (active == tcb)
    dispatch_impl(); // noreturn
  lock.release();
}

void scheduler_t::yield() {
  lock.acquire();
  assert(active->status != Sleeping);
  // A thread can be either in Init or Ready here.
  if (active->status != Ready) {
    ready.push_back(active);
    active->status = Ready;
  }
  dispatch_impl();
}

void scheduler_t::prepare_to_sleep() {
  synchronized _(lock);
  if (active->status == Sleeping)
    return;
  sleep.push_back(active);
  active->status = Sleeping;
}

void scheduler_t::maybe_preempt_impl() {
  // Preempt the idle pcb immediately. Don't wait till timer fire.
  if (active->pcb->pid == 0) {
    ready.push_back(active);
    active->status = Ready;
    lock.release();
    // This automatically calls dispatch().
    context_save(&active->ctx, &active->ctx_valid);
  } else
    lock.release();
}

void scheduler_t::wakeup(tcb_t *tcb, bool can_preempt) {
  lock.acquire();
  assert(tcb->status == Sleeping);
  tcb->status = Ready;
  sleep.erase(tcb);
  ready.push_back(tcb);
  if (can_preempt)
    maybe_preempt_impl();
  else
    lock.release();
}

void scheduler_t::tick() {
  for (auto entry = napping.q.front(); entry != nullptr;) {
    auto next = entry->next;
    if (--entry->tcb->timeout <= 0)
      napping.wake(*entry, /*can_preempt=*/ false);

    entry = next;
  }
  for (auto pcb : *itimer_real) {
    auto &itimer = pcb->itimers[ITIMER_REAL];
    if (--itimer.timeout <= 0) {
      pcb->send_signal(SIGALRM);
      if (itimer.interval != 0)
        itimer.timeout = itimer.interval;
      // TODO: otherwise, remove.
    }
  }
}

void scheduler_t::record_itimer_real(pcb_t *pcb) {
  itimer_real->push_back(pcb);
}

}
