#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "pcb.h"
#include "../lock/lock.h"

namespace os {

struct scheduler_t {
  os::intrusive_list<tcb_t> sleep, ready;
  tcb_t *active = nullptr;
  spinlock *lock = nullptr;

  // No global constructor is allowed. Hence this explicit init.
  void init() { lock = new spinlock; }
  
  void add(tcb_t *tcb);
  // Chooses the next process to schedule, and switches to it.
  [[noreturn]] void dispatch();
  void erase(tcb_t *tcb);

  // Puts the current active process to sleep.
  // When sleepy = false, puts it to ready state instead.
  [[noreturn]] void yield(bool sleepy = true);

  void wakeup(tcb_t *tcb);
private:
  [[noreturn]] void dispatch_impl();
};

static_assert(offsetof(scheduler_t, active) == 48);

extern scheduler_t scheduler;
extern static_storage<tcb_t> boot_tcb;
extern static_storage<pcb_t> boot_pcb;

inline tcb_t *active() {
  return scheduler.active;
}

}

#endif
