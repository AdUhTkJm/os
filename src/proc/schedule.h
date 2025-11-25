#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "pcb.h"
#include "../lock/lock.h"

namespace os {

struct scheduler_t {
  os::intrusive_list<pcb_t> sleep, ready;
  pcb_t *active = nullptr;
  spinlock *lock = nullptr;

  // No global constructor is allowed. Hence this explicit init.
  void init() { lock = new spinlock; pid_map_s.construct(); }
  
  void add(pcb_t *pcb);
  // Chooses the next process to schedule, and switches to it.
  void dispatch();
  void erase(pcb_t *pcb);

  // Puts the current active process to sleep.
  // When sleepy = false, puts it to ready state instead.
  void yield(bool sleepy = true);

  void wakeup(pcb_t *pcb);
private:
  void dispatch_impl();
};

static_assert(offsetof(scheduler_t, active) == 48);

extern scheduler_t scheduler;
extern static_storage<pcb_t> boot_pcb;

}

#endif
