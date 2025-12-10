#ifndef SYSRET_H
#define SYSRET_H

#include <stddef.h>

// Note these are not in namespace os.
// These are returned structures from system call, or macros for different arguments.
//
// These are typically directly taken from linux headers.

#define AT_FDCWD -100

// See <time.h>

/* Identifier for system-wide realtime clock.  */
# define CLOCK_REALTIME			0
/* Monotonic system-wide clock.  */
# define CLOCK_MONOTONIC		1
/* High-resolution timer from the CPU.  */
# define CLOCK_PROCESS_CPUTIME_ID	2
/* Thread-specific CPU-time clock.  */
# define CLOCK_THREAD_CPUTIME_ID	3
/* Monotonic system-wide clock, not adjusted for frequency scaling.  */
# define CLOCK_MONOTONIC_RAW		4
/* Identifier for system-wide realtime clock, updated only on ticks.  */
# define CLOCK_REALTIME_COARSE		5
/* Monotonic system-wide clock, updated only on ticks.  */
# define CLOCK_MONOTONIC_COARSE		6
/* Monotonic system-wide clock that includes time spent in suspension.  */
# define CLOCK_BOOTTIME			7
/* Like CLOCK_REALTIME but also wakes suspended system.  */
# define CLOCK_REALTIME_ALARM		8
/* Like CLOCK_BOOTTIME but also wakes suspended system.  */
# define CLOCK_BOOTTIME_ALARM		9
/* Like CLOCK_REALTIME but in International Atomic Time.  */
# define CLOCK_TAI			11

struct timespec {
  long tv_sec;
  long tv_nsec;
};

// See <dirent.h>
struct linux_dirent64 {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  unsigned char type;
  char name[];
};

// See <linux/futex.h>
struct robust_list {
	struct robust_list *next;
};

struct robust_list_head {
	struct robust_list list;
	long futex_offset;
	struct robust_list *list_op_pending;
};

// See <signal.h>
#define	SIG_BLOCK     0		 /* Block signals.  */
#define	SIG_UNBLOCK   1		 /* Unblock signals.  */
#define	SIG_SETMASK   2		 /* Set the set of blocked signals.  */

union sigval_t {
  int sival_int;
  void *sival_ptr;
};

typedef struct {
  int si_signo;		/* Signal number.  */
  int si_errno;		/* If non-zero, an errno value associated with
          this signal, as defined in <errno.h>.  */
  int si_code;		/* Signal code.  */
  int __pad0;			/* Explicit padding.  */

  union {
    int _pad[28];

    /* kill().  */
    struct {
      int si_pid;	/* Sending process ID.  */
      int si_uid;	/* Real user ID of sending process.  */
    } _kill;

    /* POSIX.1b timers.  */
    struct {
      int si_tid;		/* Timer ID.  */
      int si_overrun;	/* Overrun count.  */
      sigval_t si_sigval;	/* Signal value.  */
    } _timer;

    /* POSIX.1b signals.  */
    struct {
      int si_pid;	/* Sending process ID.  */
      int si_uid;	/* Real user ID of sending process.  */
      sigval_t si_sigval;	/* Signal value.  */
    } _rt;

    /* SIGCHLD.  */
    struct {
      int si_pid;	/* Which child.	 */
      int si_uid;	/* Real user ID of sending process.  */
      int si_status;	/* Exit value or signal.  */
      unsigned long si_utime;
      unsigned long si_stime;
    } _sigchld;

    /* SIGILL, SIGFPE, SIGSEGV, SIGBUS.  */
    struct {
      void *si_addr;	    /* Faulting insn/memory ref.  */
      short si_addr_lsb;  /* Valid LSB of the reported address.  */
      union {
        /* used when si_code=SEGV_BNDERR */
        struct {
          void *_lower;
          void *_upper;
        } _addr_bnd;
        /* used when si_code=SEGV_PKUERR */
        unsigned _pkey;
      } _bounds;
    } _sigfault;

    /* SIGPOLL.  */
    struct {
      long si_band;	/* Band event for SIGPOLL.  */
      int si_fd;
    } _sigpoll;

    /* SIGSYS.  */
    struct {
      void *_call_addr;	/* Calling user insn.  */
      int _syscall;	/* Triggering system call number.  */
      unsigned int _arch; /* AUDIT_ARCH_* of syscall.  */
    } _sigsys;
  } _sifields;
} siginfo_t;


/* X/Open requires some more fields with fixed names.  */
#define si_pid		_sifields._kill.si_pid
#define si_uid		_sifields._kill.si_uid
#define si_timerid	_sifields._timer.si_tid
#define si_overrun	_sifields._timer.si_overrun
#define si_status	_sifields._sigchld.si_status
#define si_utime	_sifields._sigchld.si_utime
#define si_stime	_sifields._sigchld.si_stime
#define si_value	_sifields._rt.si_sigval
#define si_int		_sifields._rt.si_sigval.sival_int
#define si_ptr		_sifields._rt.si_sigval.sival_ptr
#define si_addr		_sifields._sigfault.si_addr
#define si_addr_lsb	_sifields._sigfault.si_addr_lsb
#define si_lower	_sifields._sigfault._bounds._addr_bnd._lower
#define si_upper	_sifields._sigfault._bounds._addr_bnd._upper
#define si_pkey		_sifields._sigfault._bounds._pkey
#define si_band		_sifields._sigpoll.si_band
#define si_fd		_sifields._sigpoll.si_fd
#define si_call_addr	_sifields._sigsys._call_addr
#define si_syscall	_sifields._sigsys._syscall
#define si_arch	_sifields._sigsys._arch

struct sigset_t {
  unsigned long val[16];
};

struct sigaction {
  void     (*sa_handler)(int);
  sigset_t   sa_mask;
  int        sa_flags;
  void     (*sa_restorer)(void);
};

/* Structure for scatter/gather I/O.  */
struct iovec {
  void *iov_base;	/* Pointer to data.  */
  size_t iov_len;	/* Length of data.  */
};

// From <sys/ioctl.h>
struct winsize {
  unsigned short int ws_row;
  unsigned short int ws_col;
  unsigned short int ws_xpixel;
  unsigned short int ws_ypixel;
};

struct termio {
  unsigned short int c_iflag;		/* input mode flags */
  unsigned short int c_oflag;		/* output mode flags */
  unsigned short int c_cflag;		/* control mode flags */
  unsigned short int c_lflag;		/* local mode flags */
  unsigned char c_line;		      /* line discipline */
  unsigned char c_cc[8];		    /* control characters */
};

struct utsname {
  char sysname[65]; 
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
};

#endif
