#include "../proc/schedule.h"

namespace os {

void sighandle() {
  auto tcb = active();
  auto pcb = tcb->pcb;

  int sig = pcb->pending.next(tcb->mask);
  if (sig == 0)
    return;

  pcb->pending.remove(sig);
  auto action = pcb->sigact[sig];
  if (!action.handler) {
    // Do the default handling.
    printk("default handle: %d\n", sig);
    os::terminate(tcb, -sig);
  }
}

}
