#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "pcb.h"

namespace os {

class scheduler_t {
  os::intrusive_list<pcb_t> sleep, ready;
  pcb_t *active;
public:
  void add(pcb_t *pcb);
  pcb_t *choose();
  void erase(pcb_t *pcb);

  pcb_t *get_active() { return active; }
};

extern scheduler_t scheduler;

}

#endif
