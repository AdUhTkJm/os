#ifndef SIG_H
#define SIG_H

namespace os {

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
  SIGSEGV  = 11,
  SIGALRM  = 14,
  SIGTERM  = 15,
  SIGSTOP  = 19,
  SIGCONT  = 20,
  SIGCHLD  = 17,
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
  void (*handler)(int) = nullptr;
  sigset mask;
  int flags;
};

}

#endif
