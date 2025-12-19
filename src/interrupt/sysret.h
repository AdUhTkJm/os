#ifndef SYSRET_H
#define SYSRET_H

#include <stddef.h>

// Note these are not in namespace os.
// These are returned structures from system call, or macros for different arguments.
//
// These are typically directly taken from linux headers.

// From <fcntl.h>
#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EMPTY_PATH		0x1000
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

// From <time.h>

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

struct timeval {
  long tv_sec;
  long tv_usec;
};

struct timezone {
  int tz_minuteswest;     /* minutes west of Greenwich */
  int tz_dsttime;         /* type of DST correction */
};

// From <dirent.h>
struct linux_dirent64 {
  unsigned long inum;
  unsigned long _resv;
  unsigned short len;
  unsigned char type;
  char name[];
};

// From <linux/futex.h>
struct robust_list {
	struct robust_list *next;
};

struct robust_list_head {
	struct robust_list list;
	long futex_offset;
	struct robust_list *list_op_pending;
};

// From <signal.h>
#define	SIG_BLOCK     0		 /* Block signals.  */
#define	SIG_UNBLOCK   1		 /* Unblock signals.  */
#define	SIG_SETMASK   2		 /* Set the set of blocked signals.  */

union sigval_t {
  int sival_int;
  void *sival_ptr;
};

struct siginfo_t {
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
};


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
  unsigned long val;
};

struct sigaction {
  void (*sa_handler)(int, siginfo_t*, void*);
  unsigned long sa_flags;
  sigset_t sa_mask;
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

// From <poll.h>
struct pollfd {
  int   fd;         /* file descriptor */
  short events;     /* requested events */
  short revents;    /* returned events */
};

// From <sys/stat.h>
struct stat {
  unsigned long st_dev;      /* ID of device containing file */
  unsigned long st_ino;      /* Inode number */
  unsigned      st_mode;     /* File type and mode */
  unsigned long st_nlink;    /* Number of hard links */
  unsigned      st_uid;      /* User ID of owner */
  unsigned      st_gid;      /* Group ID of owner */
  unsigned long st_rdev;     /* Device ID (if special file) */
           long st_size;     /* Total size, in bytes */
           long st_blksize;  /* Block size for filesystem I/O */
           long st_blocks;   /* Number of 512 B blocks allocated */

  struct timespec st_atim;  /* Time of last access */
  struct timespec st_mtim;  /* Time of last modification */
  struct timespec st_ctim;  /* Time of last status change */
};

// From <sys/wait.h>
#define	WNOHANG		1	/* Don't block waiting.  */
#define	WUNTRACED	2	/* Report status of stopped children.  */

// From <unistd.h>
#define	R_OK	4		/* Test for read permission.  */
#define	W_OK	2		/* Test for write permission.  */
#define	X_OK	1		/* Test for execute permission.  */
#define	F_OK	0		/* Test for existence.  */

#define	S_IFDIR	0040000	/* Directory.  */
#define	S_IFCHR	0020000	/* Character device.  */
#define	S_IFBLK	0060000	/* Block device.  */
#define	S_IFREG	0100000	/* Regular file.  */
#define	S_IFIFO	0010000	/* FIFO.  */
#define	S_IFLNK	0120000	/* Symbolic link.  */
#define	S_IFSOCK	0140000	/* Socket.  */

// From <sys/socket.h>
#define AF_UNSPEC	0	/* Unspecified.  */
#define AF_LOCAL	1	/* Local to host (pipes and file-domain).  */
#define AF_UNIX		AF_LOCAL /* POSIX name for PF_LOCAL.  */
#define AF_FILE		AF_LOCAL /* Another non-standard name for PF_LOCAL.  */
#define AF_INET		2	/* IP protocol family.  */

#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOCK_RAW     3

struct sockaddr {
  unsigned short sa_family;
  char           sa_data[14];
};

// From <netinet/in.h>
struct in_addr {
  unsigned int s_addr;
};

struct sockaddr_in {
  short          sin_family;
  unsigned short sin_port;
  struct in_addr sin_addr;
  // 8 byte unused
};

// From man syslog(2); the constants aren't defined in headers as far as I know.
#define SYSLOG_ACTION_CLOSE       0
#define SYSLOG_ACTION_OPEN        1
#define SYSLOG_ACTION_READ        2
#define SYSLOG_ACTION_READ_ALL    3
#define SYSLOG_ACTION_READ_CLEAR  4
#define SYSLOG_ACTION_CLEAR       5
#define SYSLOG_ACTION_CONSOLE_OFF 6
#define SYSLOG_ACTION_CONSOLE_ON  7
#define SYSLOG_ACTION_CONSOLE_LEVEL 8
#define SYSLOG_ACTION_SIZE_UNREAD 9
#define SYSLOG_ACTION_SIZE_BUFFER 10

#endif
