#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "pcb.h"
#include "../lock/lock.h"

namespace os {

// These are processes that are sleeping for a timeout.
extern static_storage<os::list<tcb_t*>> napping;

struct scheduler_t {
  os::intrusive_list<tcb_t> sleep, ready;
  tcb_t *active = nullptr;
  spinlock lock;

  // No global constructor is allowed. Hence this explicit init.
  void init() { napping.construct(); }
  
  void add(tcb_t *tcb);
  // Chooses the next process to schedule, and switches to it.
  [[noreturn]] void dispatch();
  void erase(tcb_t *tcb);

  // Puts the current active process to the ready queue, and immediately dispatch.
  [[noreturn]] void yield();
  // Puts the current active process to the sleep queue, but does not dispatch.
  // This is idempotent.
  void prepare_to_sleep();

  // Note that even if `can_preempt` is true, it doesn't mean preemption will always happen.
  void wakeup(tcb_t *tcb, bool can_preempt = true);

  // Wakes up everything in this container, and clears it.
  // We need the lock to protect the write to tcb.
  template<locklike Lock, typename Vector> requires requires(Vector v) {
    v.clear();
    v.end(); v.begin();
  }
  void wakeup_all(Lock &vlock, Vector &tcbs, bool can_preempt = true) {
    lock.acquire();
    {
      synchronized __(vlock);

      for (auto tcb : tcbs) {
        tcb->status = Ready;
        sleep.erase(tcb);
        ready.push_back(tcb);
      }
      tcbs.clear();
    }
    if (can_preempt)
      maybe_preempt_impl();
    else
      lock.release();
  }

  void maybe_preempt() {
    lock.acquire();
    maybe_preempt_impl();
  }

  // Remove tcb from the napping list.
  void unnap(tcb_t *tcb, bool wake = true);
  // Tell all napping processes that they have already napped a timer tick.
  // Wakes up processes, but doesn't preempt.
  void tick();
private:
  [[noreturn]] void dispatch_impl();
  
  void maybe_preempt_impl();
};

static_assert(offsetof(scheduler_t, active) == 48);

extern scheduler_t scheduler;
extern static_storage<tcb_t> boot_tcb;
extern static_storage<pcb_t> boot_pcb;

inline tcb_t *active() {
  return scheduler.active;
}

constexpr int tick_length = 100_ms;

}

#endif
