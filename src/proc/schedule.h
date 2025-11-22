#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "pcb.h"

namespace os {

// In order to let assembly access it, we should make it standard-layout.
// So we're not using `private` here even if we should do.
struct scheduler_t {
  os::intrusive_list<pcb_t> sleep, ready;
  pcb_t *active;
  
  void add(pcb_t *pcb);
  // Choose the next process to schedule.
  // Does not modify `active`.
  pcb_t *choose();
  void erase(pcb_t *pcb);
};

static_assert(offsetof(scheduler_t, active) == 48);

extern scheduler_t scheduler;
extern static_storage<pcb_t> boot_pcb;

}

#endif
