#include "../proc/schedule.h"

namespace os {

void kill(int sig) {
  os::terminate(active(), -sig);
}

void core(int sig) {
  kill(sig);
}

void sighandle() {
  auto tcb = active();
  auto pcb = tcb->pcb;
  if (tcb->kmode) {
    size_t time = now();
    tcb->ruse.ru_stime += time - tcb->last_sched;
    tcb->last_sched = time;
    tcb->kmode = false;
  }

  int sig = tcb->pending.next(tcb->mask);
  if (sig != 0)
    tcb->pending.remove(sig);
  else if ((sig = pcb->pending.next(tcb->mask)) != 0)
    pcb->pending.remove(sig);
  if (sig == 0)
    return;

  tcb->sigresume = -1;
  if (sig == SIGKILL) {
    // Don't invoke handler. Just terminate.
    os::terminate(pcb, -sig);
    return;
  }

  auto action = pcb->sigact[sig];
  // TODO: implement custom handler
  if (1 || !action.handler) {
    switch (sig) {
    case SIGABRT:
    case SIGFPE:
    case SIGILL:
    case SIGQUIT:
    case SIGSEGV:
      core(sig);
      break;

    case SIGUSR1:
    case SIGUSR2:
      kill(sig);

    case SIGCHLD:
      // Ignore
      break;

    default:
      printk("unknown signal: %d\n", sig);
      os::terminate(tcb, -sig);
    }
    return;
  }
  
  printk("handler: %p\n", action.handler);
  assert(false && "no custom handlers yet!");
}

}
