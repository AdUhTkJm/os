#include "../proc/schedule.h"
#include "sysids.h"

namespace os {

// Some system calls can't be retried.
constexpr syscall norestart[] = {
  flock
};

bool restartable(int v) {
  for (auto num : norestart) {
    if (v == num)
      return false;
  }
  return true;
}

void kill(int sig) {
  os::terminate(active(), sig, true);
}

void core(int sig) {
  kill(sig);
}

void sighandle() {
  auto tcb = active();
  auto pcb = tcb->pcb;
  bool sysret = tcb->sysret;
  tcb->sysret = false;

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
    os::terminate(pcb, sig, true);
    return;
  }

  auto action = pcb->actor->sigact[sig];
  if (!action.handler) {
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
    case SIGPIPE:
      kill(sig);

    case SIGCHLD:
      // Ignore
      break;

    default:
      printk("unknown signal: %d\n", sig);
      os::terminate(tcb, sig, true);
    }
    return;
  }
  // SIG_IGN: we should ignore the signal.
  if ((va_t) action.handler == 1)
    return;
  
  // Set up a trapframe in user space.
  // The frame is as follows:
  //
  // usp:
  //   08b00893  li a7, 139
  //   00000073  ecall
  //
  // Then we need to set `ra` to usp.
#ifdef RV
  auto trap = (trapframe *) tcb->ksp;
  memcpy(&tcb->sigf, trap, sizeof(trapframe));
  if ((action.flags & SA_RESTART) && sysret && trap->regs[8] == -EINTR && restartable(trap->regs[15])) {
    tcb->sigf.regs[8] = tcb->a0;
    tcb->sigf.sepc -= 4;
  }

  char *usp = (char *) trap->sscratch;

  usp -= 16;
  unsigned insn = 0x08b00893;
  (void) copy_to_user(usp, &insn, 4);
  insn = 0x73;
  (void) copy_to_user(usp + 4, &insn, 4);
  // TODO: use a separate page instead.
  os::pmap(to_pa(usp), usp, MAP_4KB, PTE_V | PTE_U | PTE_RWX, pt_root());

  trap->regs[/*ra*/ 0] = (va_t) usp;
  trap->regs[/*a0*/ 8] = sig;
  trap->sscratch = (va_t) usp;
  trap->sepc = (va_t) action.handler;
  __asm__ volatile("fence.i");
#endif

#ifdef LA
  assert(false && "no signals yet!");
#endif
}

}
