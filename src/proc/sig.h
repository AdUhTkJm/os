#ifndef SIG_H
#define SIG_H

struct siginfo_t;

namespace os {

// See man signal(7). It can also be found online here:
// https://man7.org/linux/man-pages/man7/signal.7.html
enum signals : unsigned char {
  SIGNONE  = 0,
  SIGHUP   = 1, 
  SIGINT   = 2,
  SIGQUIT  = 3,
  SIGILL   = 4,
  SIGTRAP  = 5,
  SIGABRT  = 6,
  SIGFPE   = 8,
  SIGKILL  = 9,
  SIGUSR1  = 10,
  SIGSEGV  = 11,
  SIGUSR2  = 12,
  SIGALRM  = 14,
  SIGTERM  = 15,
  SIGCHLD  = 17,
  SIGCONT  = 18,
  SIGSTOP  = 19,
  SIGTSTP  = 20,
};

struct sigset {
  unsigned long sig;
  
  constexpr static int sigmax = sizeof(sig);
  sigset() = default;
  /* implicit */ sigset(unsigned long sig): sig(sig) {}

  void add(int signum);
  void remove(int signum);

  bool operator[](int signum) const;
  int next(const sigset &ignored) const;
};

struct sigaction {
  void (*handler)(int, siginfo_t*, void*) = nullptr;
  sigset mask = 0;
  unsigned long flags = 0;
};

}

#endif
