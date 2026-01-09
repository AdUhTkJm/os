#ifndef PCB_H
#define PCB_H

#include "sig.h"
#include "../utils/helper.h"
#include "../mem/ptable.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"
#include "../utils/stl/optional.h"
#include "../fs/initramfs.h"
#include "../interrupt/sysret.h"

// Taken from <sched.h>

#define CLONE_VM      0x00000100 /* Set if VM shared between processes.  */
#define CLONE_FS      0x00000200 /* Set if fs info shared between processes.  */
#define CLONE_FILES   0x00000400 /* Set if open files shared between processes.  */
#define CLONE_SIGHAND 0x00000800 /* Set if signal handlers shared.  */
#define CLONE_PIDFD   0x00001000 /* Set if a pidfd should be placed in parent.  */
#define CLONE_PTRACE  0x00002000 /* Set if tracing continues on the child.  */
#define CLONE_VFORK   0x00004000 /* Set if the parent wants the child to wake it up on mm_release.  */
#define CLONE_PARENT  0x00008000 /* Set if we want to have the same parent as the cloner.  */
#define CLONE_THREAD  0x00010000 /* Set to add to same thread group.  */
#define CLONE_NEWNS   0x00020000 /* Set to create new namespace.  */
#define CLONE_SYSVSEM 0x00040000 /* Set to shared SVID SEM_UNDO semantics.  */
#define CLONE_SETTLS  0x00080000 /* Set TLS info.  */
#define CLONE_PARENT_SETTID 0x00100000 /* Store TID in userlevel buffer before MM copy.  */
#define CLONE_CHILD_CLEARTID 0x00200000 /* Register exit futex and memory location to clear.  */
#define CLONE_DETACHED 0x00400000 /* Create clone detached.  */
#define CLONE_UNTRACED 0x00800000 /* Set if the tracing process can't force CLONE_PTRACE on this clone.  */
#define CLONE_CHILD_SETTID 0x01000000 /* Store TID in userlevel buffer in the child.  */
#define CLONE_NEWCGROUP    0x02000000	/* New cgroup namespace.  */
#define CLONE_NEWUTS	0x04000000	/* New utsname group.  */
#define CLONE_NEWIPC	0x08000000	/* New ipcs.  */
#define CLONE_NEWUSER	0x10000000	/* New user namespace.  */
#define CLONE_NEWPID	0x20000000	/* New pid namespace.  */
#define CLONE_NEWNET	0x40000000	/* New network namespace.  */
#define CLONE_IO	0x80000000	/* Clone I/O context.  */
#define CLONE_NEWTIME	0x00000080  /* New time namespace */

namespace os::tty {

struct tty;

}

namespace os {

#define __user

constexpr size_t user_stack_size = 8_mb;
constexpr size_t kstack_size = 8_kb - 16;

enum thread_state {
  Init, Running, Sleeping, Ready, Zombie, Dead
};

#ifdef RV
struct trapframe {
  reg_t regs[30]; // zero and sp are not stored.
  reg_t sscratch; // this is the user sp.
  reg_t sepc;
  reg_t sstatus;
  char pad[8];
};
#endif
#ifdef LA
struct trapframe {
  reg_t regs[30];
  reg_t sscratch; // Actually, this is user sp. We didn't store it in `regs` above.
  reg_t sepc;     // Actually, this is `era` in Loongarch.
  reg_t prmd;
  reg_t euen;
};
#endif

// This is to resume at the place exactly AFTER the call to suspend().
// The code would not expect we preserve caller-saved registers, so don't preserve them.
// Moreover, as we entered suspend(), we know the return address register (ra) is exactly what we want:
// the next instruction to execute.
struct ctxframe {
  reg_t regs[12]; // The callee-saved (s*) registers.
  reg_t pc;
  reg_t sp;       // Kernel sp.
  reg_t sepc;
  reg_t sstatus;
};

using sigframe = trapframe;

#ifdef __cplusplus
static_assert(sizeof(trapframe) == 272);
#endif

class process_file_table : public shared {
public:
  using fddesc = unsigned char;
private:
  // The open file table.
  os::hashmap<int, file*> open;
  // The file descriptor table.
  os::hashmap<int, fddesc> desc;
public:
  process_file_table() = default;
  process_file_table(const process_file_table &);
  process_file_table &operator=(const process_file_table &);
  ~process_file_table() { clear(); }
  
  int allocate(file *f, int fd = -1);
  // The semantics is like F_DUPFD, where `fd` isn't exact as in dup3, but a start-to-search point.
  int allocate_from(file *f, int fd);
  void deallocate(int fd);
  void clear();
  int count(int fd) { return open.count(fd); }
  size_t size() { return open.size(); }
  
  // It is sometimes not easy to use `operator[]` on a pointer.
  file *operator[](int x) { return open.count(x) ? open[x] : nullptr; }
  file *at(int x) { return open.count(x) ? open[x] : nullptr; }

  void set_desc(int fd, fddesc desc) { this->desc[fd] = desc; }
  optional<fddesc> get_desc(int fd) { return desc.count(fd) ? optional(desc[fd]) : nullopt; }
  
  using iterator = decltype(open)::iterator;
  iterator begin() { return open.begin(); }
  iterator end() { return open.end(); }
};

struct pcb_t;
struct tcb_t : os::intrusive_list_node<tcb_t> {
  int tid;                // Thread id.
  thread_state status;    // Thread status (running, sleeping etc.)
  va_t ksp;               // Kernel stack for this thread.
  bool ctx_valid = false; // Whether the syscall/trap context is valid.
  bool kthread = false;   // Whether this is a kernel thread.
  bool kmode = false;     // Whether this executed in kernel mode.
  bool sysret = false;    // Whether the thread is returning from a system call.
  bool intr = true;       // Whether the thread is interruptible from the current sleep.
  unsigned char sclock;   // The clock that the thread sleeps on.
  reg_t a0;               // The previous a0, when returning from a system call.
  ctxframe ctx;           // Context frame for blocking syscalls / context switch.
  int ret;                // Thread return code.
  int sigresume = 0;      // The signal that causes the thread to wake up from sigwait().
  void *stidaddr = 0;     // The tid address for `settid` to operate on.
  void *ctidaddr = 0;     // The tid address for `cleartid` to operate on.
  sigset mask = 0;        // Masked (ignored) signals.
  sigset pending = 0;     // Pending signals, for this thread.
  sigset sigwait = 0;     // Signals that the process is waiting for.
  long timeout = 0;       // Timeout to last sleep.
  long last_sched = 0;    // Time before last schedule.
  pusage ruse {};         // Resource usage.
  sigframe sigf;          // The signal frame, for sigreturn().
  hashmap<wait_entry*, wait_queue*> entr;

  pcb_t *pcb;             // Parent process.

  void send_signal(int sig);

  // This is the final deletion. This cannot be done in clear(), because it
  // is called when this ksp is in use.
  ~tcb_t() {
    auto ksp_bottom = ksp + sizeof(trapframe) - kstack_size;
    vfree((void*) ksp_bottom);
  }

  int sleep(size_t nano);
};

struct pcb_t {
  pa_t pt_root;           // Root page table entry.
  vma::addrspace *vma;    // Virtual memory areas.
  pcb_t *parent;          // Parent.
  process_file_table*ftbl;// Process file table.
  int uid, euid, suid;
  int gid, egid, sgid;
  int pid, pgid, sid;
  bool kproc = false;     // Kernel process.
  bool execd = false;     // Has performed `exec`.
  bool zombie = false;    // Whether this is a zombie.
  bool sigterm;           // Whether the process terminated because of a signal.
  os::list<tcb_t*> threads;
  os::vector<pcb_t*> children;
  class vfs *vfs;
  void *robust_list;      // Futex list that should wake up threads waiting on it, on process exit.
  int ret;                // Process return code.
  int umask = 022;        // Mask on mode when creating file.
  int sigonterm = SIGCHLD;// The signal to send to parent on termination.
  dentry *pwd;            // Process working directory.
  string execpath;        // The path to the executable.
  sigset pending = 0;     // Pending signals.
  sigaction sigact[32];   // Signal actions.
  os::tty::tty *tty;      // Terminal typewriter.
  wait_queue wait;        // Threads suspended in wait() system call.
  wait_queue vfork;       // Threads suspended in vfork() system call (or clone, clone3 with CLONE_VFORK).
  spinlock waitlock;      // Lock associated with `wait`.
  spinlock vforklock;     // Lock associated with `vfork`.
  rlimit rlims[9];        // Resource limits.
  pusage cruse;           // The rusage of all terminated children by wait().
  struct itimer {
    long timeout;         // In ticks.
    long interval;        // In ticks.
  } itimers[3] {};        // Timers for get/setitimer() system call.

  // Note this is not the destructor. PCB will need to release its resources
  // before destruction, and then put itself to a zombie state.
  void clear();
  pcb_t();
  ~pcb_t();

  expected<dentry*> obtain_file(const string &name, int dirfd, int flags);
  expected<dentry*> obtain_file_emptyable(const string &name, int dirfd, int flags);
  int open_file_from(const string &name, dentry *relbase, int flags, int mode, inode::filetype type);
  int open_file_from(const string &name, int dirfd, int flags, int mode = 0, inode::filetype type = inode::File);
  int open_file(const string &name, int flags, int mode = 0, inode::filetype type = inode::File);
  int close_file(int fd);

  void send_signal(int sig);
};
/*
Note for ksp:
  It actually does not point to top of the kernel stack.
  It points to the place after trap frame, i.e.

  [ra tp gp ... t6 sepc sstatus | <stack>]
                                |----------- ksp
*/

static_assert(offsetof(tcb_t, ksp) == 24);

extern static_storage<hashmap<int, pcb_t*>> pidmap;

// We can terminate a thread or a process.
void terminate(tcb_t *tcb, int ret, bool sig);
void terminate(pcb_t *pcb, int ret, bool sig);

// Set up the returning from the trap handler.
void trap_return_setup(tcb_t *pcb);

// Suspend the current system call.
void suspend();

// Gives the next free pid.
int nextpid();

// Forks a process.
tcb_t *clone(unsigned flags, va_t usp, void *tls, void *childtid);

// Replaces a process image.
int exec(const string &path, const vector<string> &argv, const vector<string> &envp);

// Even though it does not return for now, it will eventually look as if it "returned".
// Return value marks whether this is interrupted by a signal (-EINTR) or a normal return (0).
class mutex;
int context_save(void *ctx, bool *ctx_valid);
extern "C" [[noreturn]] void context_restore(void *ctx, bool from_signal);

#define suspend() context_save(&active()->ctx, &active()->ctx_valid)

// Creates a kernel process.
template<class T> requires (is_function_v<remove_pointer_t<T>>)
tcb_t *make_kprocess(T fptr) {
  pcb_t *pcb = new pcb_t;
  tcb_t *tcb = new tcb_t;
  pcb->pid = pcb->pgid = pcb->sid = nextpid();
  pcb->kproc = true;
  pcb->gid = pcb->uid = 0;
  pcb->egid = pcb->euid = pcb->sgid = pcb->suid = 0;
  pcb->parent = nullptr;
  
  pcb->vfs = new vfs;
  pcb->vfs->base = initramfs->root;
  pcb->vfs->ref();

  pcb->ftbl = new process_file_table;
  pcb->ftbl->ref();

  pcb->vma = new vma::addrspace;
  pcb->vma->ref();

  tcb->status = Init;
  // We aren't lazy-allocating here.
  // Also don't forget that stack grows downwards.
  constexpr auto usp_size = 16_kb - 16;
  tcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  auto trap = (trapframe *) tcb->ksp;
  trap->sscratch = (va_t) vmalloc<16>(usp_size) + usp_size;
  trap->sepc = (va_t) fptr;

  tcb->pcb = pcb;
  tcb->tid = pcb->pid;
  pcb->rlims[RLIMIT_STACK].rlim_cur = pcb->rlims[RLIMIT_STACK].rlim_max = usp_size;
  pcb->threads.push_back(tcb);
  pidmap->insert(pcb->pid, pcb);
  pcb->pt_root = __kernel_pt_root;
  return tcb;
}

}

#endif
