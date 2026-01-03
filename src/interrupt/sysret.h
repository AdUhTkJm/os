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
#define AT_REMOVEDIR		0x200
#define AT_EMPTY_PATH		0x1000
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

// From <time.h>

/* Identifier for system-wide realtime clock.  */
#define CLOCK_REALTIME			0
/* Monotonic system-wide clock.  */
#define CLOCK_MONOTONIC		1
/* High-resolution timer from the CPU.  */
#define CLOCK_PROCESS_CPUTIME_ID	2
/* Thread-specific CPU-time clock.  */
#define CLOCK_THREAD_CPUTIME_ID	3
/* Monotonic system-wide clock, not adjusted for frequency scaling.  */
#define CLOCK_MONOTONIC_RAW		4
/* Identifier for system-wide realtime clock, updated only on ticks.  */
#define CLOCK_REALTIME_COARSE		5
/* Monotonic system-wide clock, updated only on ticks.  */
#define CLOCK_MONOTONIC_COARSE		6
/* Monotonic system-wide clock that includes time spent in suspension.  */
#define CLOCK_BOOTTIME			7
/* Like CLOCK_REALTIME but also wakes suspended system.  */
#define CLOCK_REALTIME_ALARM		8
/* Like CLOCK_BOOTTIME but also wakes suspended system.  */
#define CLOCK_BOOTTIME_ALARM		9
/* Like CLOCK_REALTIME but in International Atomic Time.  */
#define CLOCK_TAI			11

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

struct tms {
  long tms_utime;  /* user time */
  long tms_stime;  /* system time */
  long tms_cutime; /* user time of children */
  long tms_cstime; /* system time of children */
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

#define SA_SIGINFO   4
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

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

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_CLOEXEC  02000000
#define SOCK_NONBLOCK 00004000
#define SOL_SOCKET	1

#define SO_DEBUG	1
#define SO_REUSEADDR	2
#define SO_TYPE		3
#define SO_ERROR	4
#define SO_DONTROUTE	5
#define SO_BROADCAST	6
#define SO_SNDBUF	7
#define SO_RCVBUF	8
#define SO_SNDBUFFORCE	32
#define SO_RCVBUFFORCE	33
#define SO_KEEPALIVE	9
#define SO_OOBINLINE	10
#define SO_NO_CHECK	11    /* No checksum */
#define SO_PRIORITY	12
#define SO_LINGER	13
#define SO_BSDCOMPAT	14
#define SO_REUSEPORT	15
#define SO_PASSCRED	16
#define SO_PEERCRED	17
#define SO_RCVLOWAT	18
#define SO_SNDLOWAT	19
#define SO_RCVTIMEO_OLD	20
#define SO_SNDTIMEO_OLD	21

#define MSG_DONTROUTE 0x04
#define MSG_DONTWAIT  0x40
#define MSG_FIN       0x200
#define MSG_SYN       0x400
#define MSG_RST       0x1000
#define MSG_NOSIGNAL  0x4000
#define MSG_FASTOPEN  0x20000000

struct msghdr {
  void         *msg_name;       /* Optional address */
  unsigned      msg_namelen;    /* Size of address */
  struct iovec *msg_iov;        /* Scatter/gather array */
  size_t        msg_iovlen;     /* Number of elements in msg_iov */
  void         *msg_control;    /* Ancillary data, see below */
  size_t        msg_controllen; /* Ancillary data buffer size */
  int           msg_flags;      /* Flags (unused) */
};

struct mmsghdr {
  msghdr   msg_hdr;
  unsigned msg_len;
};

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

// From <linux/futex.h>.
#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_FD		  2
#define FUTEX_REQUEUE	3
#define FUTEX_CMP_REQUEUE	4
#define FUTEX_WAKE_OP		5
#define FUTEX_LOCK_PI		6
#define FUTEX_UNLOCK_PI		7
#define FUTEX_TRYLOCK_PI	8
#define FUTEX_WAIT_BITSET	9
#define FUTEX_WAKE_BITSET	10
#define FUTEX_WAIT_REQUEUE_PI	11
#define FUTEX_CMP_REQUEUE_PI	12
#define FUTEX_LOCK_PI2		13

#define FUTEX_PRIVATE_FLAG	128
#define FUTEX_CLOCK_REALTIME	256

// From <sys/resource.h>.
#define RLIMIT_FSIZE 1
#define	RLIMIT_DATA  2
#define RLIMIT_STACK 3
#define RLIMIT_CORE  4
#define RLIMIT_NPROC 6
#define RLIMIT_OFILE 7
#define RLIMIT_MEMLOCK 8

struct rlimit {
  unsigned long rlim_cur;  /* Soft limit */
  unsigned long rlim_max;  /* Hard limit (ceiling for rlim_cur) */
};

struct rusage {
  struct timeval ru_utime; /* user CPU time used */
  struct timeval ru_stime; /* system CPU time used */
  long   ru_maxrss;        /* maximum resident set size */
  long   ru_ixrss;         /* integral shared memory size */
  long   ru_idrss;         /* integral unshared data size */
  long   ru_isrss;         /* integral unshared stack size */
  long   ru_minflt;        /* page reclaims (soft page faults) */
  long   ru_majflt;        /* page faults (hard page faults) */
  long   ru_nswap;         /* swaps */
  long   ru_inblock;       /* block input operations */
  long   ru_oublock;       /* block output operations */
  long   ru_msgsnd;        /* IPC messages sent */
  long   ru_msgrcv;        /* IPC messages received */
  long   ru_nsignals;      /* signals received */
  long   ru_nvcsw;         /* voluntary context switches */
  long   ru_nivcsw;        /* involuntary context switches */
};

// This is for storing in PCB, and not taken from any header.
struct pusage {
  long   ru_utime;         /* user CPU time used */
  long   ru_stime;         /* system CPU time used */
  long   ru_maxrss;        /* maximum resident set size */
  long   ru_minflt;        /* page reclaims (soft page faults) */
  long   ru_majflt;        /* page faults (hard page faults) */
  long   ru_inblock;       /* block input operations */
  long   ru_oublock;       /* block output operations */
  long   ru_nvcsw;         /* voluntary context switches */
  long   ru_nivcsw;        /* involuntary context switches */

  pusage &operator+=(const pusage &other) {
    ru_utime += other.ru_utime;
    ru_stime += other.ru_stime;
    ru_maxrss = ru_maxrss > other.ru_maxrss ? ru_maxrss : other.ru_maxrss;
    ru_minflt += other.ru_minflt;
    ru_majflt += other.ru_majflt;
    ru_inblock += other.ru_inblock;
    ru_oublock += other.ru_oublock;
    ru_nvcsw += other.ru_nvcsw;
    ru_nivcsw += other.ru_nivcsw;
    return *this;
  }

  operator struct rusage() {
    return rusage {
      .ru_utime = { .tv_sec = ru_utime / 1'000'000'000, .tv_usec = (ru_utime % 1'000'000'000) / 1000 },
      .ru_stime = { .tv_sec = ru_utime / 1'000'000'000, .tv_usec = (ru_utime % 1'000'000'000) / 1000 },
      .ru_maxrss = ru_maxrss,
      .ru_ixrss = 0,
      .ru_idrss = 0,
      .ru_isrss = 0,
      .ru_minflt = ru_minflt,
      .ru_majflt = ru_majflt,
      .ru_nswap = 0,
      .ru_inblock = ru_inblock,
      .ru_oublock = ru_oublock,
      .ru_msgsnd = 0,
      .ru_msgrcv = 0,
      .ru_nsignals = 0,
      .ru_nvcsw = ru_nvcsw,
      .ru_nivcsw = ru_nivcsw,
    };
  }
};

// From <sys/time.h>.
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

struct itimerval {
  timeval it_interval;
  timeval it_value;
};

// From <sys/sysinfo.h>.
struct sysinfo {
  long uptime;             /* Seconds since boot */
  unsigned long loads[3];  /* 1, 5, and 15 minute load averages */
  unsigned long totalram;  /* Total usable main memory size */
  unsigned long freeram;   /* Available memory size */
  unsigned long sharedram; /* Amount of shared memory */
  unsigned long bufferram; /* Memory used by buffers */
  unsigned long totalswap; /* Total swap space size */
  unsigned long freeswap;  /* Swap space still available */
  unsigned short procs;    /* Number of current processes */
  unsigned long totalhigh; /* Total high memory size */
  unsigned long freehigh;  /* Available high memory size */
  unsigned int mem_unit;   /* Memory unit size in bytes */
};

// From <sched.h>.
typedef struct {
  int bits[1024 / sizeof(int)];
} cpu_set_t;

struct clone_args {
  unsigned long flags;        /* Flags bit mask */
  unsigned long pidfd;        /* Where to store PID file descriptor */
  unsigned long child_tid;    /* Where to store child TID, in child's memory */
  unsigned long parent_tid;   /* Where to store child TID, in parent's memory */
  unsigned long exit_signal;  /* Signal to deliver to parent on child termination */
  unsigned long stack;        /* Pointer to lowest byte of stack */
  unsigned long stack_size;   /* Size of stack */
  unsigned long tls;          /* Location of new TLS */
  unsigned long set_tid;      /* Pointer to a pid_t array */
  unsigned long set_tid_size; /* Number of elements in set_tid */
  unsigned long cgroup;       /* File descriptor for target cgroup */
};

// From <linux/capability.h>.
struct cap_header {
  unsigned version;
  int      pid;
};

struct cap_data {
  unsigned effective;
  unsigned permitted;
  unsigned inheritable;
};

#define LINUX_CAPABILITY_VERSION_1 0x19980330
#define LINUX_CAPABILITY_VERSION_2 0x20071026
#define LINUX_CAPABILITY_VERSION_3 0x20080522

// From <sys/ipc.h>.

/* Mode bits for `msgget', `semget', and `shmget'.  */
#define IPC_CREAT	01000		/* Create key if key does not exist. */
#define IPC_EXCL	02000		/* Fail if key exists.  */
#define IPC_NOWAIT	04000		/* Return error on wait.  */

/* Control commands for `msgctl', `semctl', and `shmctl'.  */
#define IPC_RMID	0		/* Remove identifier.  */
#define IPC_SET		1		/* Set `ipc_perm' options.  */
#define IPC_STAT	2		/* Get `ipc_perm' options.  */
#define IPC_INFO	3		/* See ipcs.  */

/* Special key values.  */
#define IPC_PRIVATE	((__key_t) 0)	/* Private key.  */

// From <sys/shm.h>.
/* Permission flag for shmget.  */
#define SHM_R		0400		/* or S_IRUGO from <linux/stat.h> */
#define SHM_W		0200		/* or S_IWUGO from <linux/stat.h> */

/* Flags for `shmat'.  */
#define SHM_RDONLY	010000		/* attach read-only else read-write */
#define SHM_RND		020000		/* round attach address to SHMLBA */
#define SHM_REMAP	040000		/* take-over region on attach */
#define SHM_EXEC	0100000		/* execution access */

/* Commands for `shmctl'.  */
#define SHM_LOCK	11		/* lock segment (root only) */
#define SHM_UNLOCK	12		/* unlock segment (root only) */

struct ipc_perm {
  int key;	   			/* Key.  */
  int uid;					/* Owner's user ID.  */
  int gid;					/* Owner's group ID.  */
  int cuid;					/* Creator's user ID.  */
  int cgid;					/* Creator's group ID.  */
  int mode;				  /* Read/write permission.  */
  unsigned short __seq;			/* Sequence number.  */
  unsigned short __pad2;
  unsigned long __resv[2];
};

struct shmid_ds {
  struct ipc_perm shm_perm;    /* Ownership and permissions */
  size_t          shm_segsz;   /* Size of segment (bytes) */
  long            shm_atime;   /* Last attach time */
  long            shm_dtime;   /* Last detach time */
  long            shm_ctime;   /* Creation time/time of last
                                  modification via shmctl() */
  int             shm_cpid;    /* PID of creator */
  int             shm_lpid;    /* PID of last shmat(2)/shmdt(2) */
  unsigned long   shm_nattch;  /* No. of current attaches */
  unsigned long   __resv[2];
};

// From <sys/select.h>.
typedef struct {
  long fds_bits[16];
#define __FDS_BITS(set) ((set)->fds_bits)
} fd_set;
#define __NFDBITS	(8 * (int) sizeof (long))
#define	__FD_ELT(d)	((d) / __NFDBITS)
#define	__FD_MASK(d)	((long) (1UL << ((d) % __NFDBITS)))
#define __FD_ZERO(s) \
  do {									      \
    unsigned int __i;							      \
    fd_set *__arr = (s);						      \
    for (__i = 0; __i < sizeof (fd_set) / sizeof (__fd_mask); ++__i)	      \
      __FDS_BITS (__arr)[__i] = 0;					      \
  } while (0)
#define __FD_SET(d, s) \
  ((void) (__FDS_BITS (s)[__FD_ELT(d)] |= __FD_MASK(d)))
#define __FD_CLR(d, s) \
  ((void) (__FDS_BITS (s)[__FD_ELT(d)] &= ~__FD_MASK(d)))
#define __FD_ISSET(d, s) \
  ((__FDS_BITS (s)[__FD_ELT (d)] & __FD_MASK (d)) != 0)

#define FD_ZERO  __FD_ZERO
#define FD_SET   __FD_SET
#define FD_CLR   __FD_CLR
#define FD_ISSET __FD_ISSET

// From <stdio.h>.
#define RENAME_NOREPLACE (1 << 0)
#define RENAME_EXCHANGE  (1 << 1)
#define RENAME_WHITEOUT  (1 << 2)

// From <sys/mman.h>.
#define MS_ASYNC      1		/* Sync memory asynchronously.  */
#define MS_SYNC       4		/* Synchronous memory sync.  */
#define MS_INVALIDATE	2		/* Invalidate the caches.  */

#endif
