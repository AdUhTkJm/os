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

namespace os::tty {

struct tty;

}

namespace os {

#define __user

// Near the highest address in the lower-half space.
constexpr va_t stack_top = 0xf'f000'0000ul;
constexpr size_t user_stack_size = 8_mb;
constexpr size_t kstack_size = 8_kb - 16;

enum thread_state {
  Init, Running, Sleeping, Ready, Zombie, Dead
};

struct trapframe {
  reg_t regs[30]; // zero and sp are not stored.
  reg_t sscratch; // this is the user sp.
  reg_t sepc;
  reg_t sstatus;
  char pad[8];
};

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
  va_t __user usp;        // User stack top for this thread.
  va_t pc;                // Initial program counter (entry point) of this thread.
  bool ctx_valid = false; // Whether the syscall/trap context is valid.
  bool kthread = false;   // Whether this is a kernel thread.
  bool kmode = false;     // Whether this executed in kernel mode.
  ctxframe ctx;           // Context frame for blocking syscalls / context switch.
  int ret;                // Thread return code.
  void __user *tls;       // Thread-local storage pointer.
  sigset mask = 0;        // Masked (ignored) signals.
  sigset pending = 0;     // Pending signals, for this thread.
  long timeout = 0;       // Timeout to last sleep.

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
  vma::addrspace vma;          // Virtual memory areas.
  pcb_t *parent;          // Parent.
  process_file_table*ftbl;// Process file table.
  int uid, euid, suid;
  int gid, egid, sgid;
  int pid, pgid, sid;
  bool kproc = false;     // Kernel process.
  bool execd = false;     // Has performed `exec`.
  bool zombie = false;    // Whether this is a zombie.
  os::intrusive_list<tcb_t> threads;
  os::vector<pcb_t*> children;
  class vfs *vfs;
  void *robust_list;      // Futex list that should wake up threads waiting on it, on process exit.
  int tidn = 0;           // Next tid.
  int ret;                // Process return code.
  int umask = 022;        // Mask on mode when creating file.
  dentry *pwd;            // Process working directory.
  string execpath;        // The path to the executable.
  sigset pending = 0;     // Pending signals.
  sigaction sigact[32];   // Signal actions.
  os::tty::tty *tty;      // Terminal typewriter.
  condvar wait;           // Threads suspended in wait() system call.
  mutex waitlock;         // Used with the previous condvar.
  tms times {};
  long last_schedule;

  // Note this is not the destructor. PCB will need to release its resources
  // before destruction, and then put itself to a zombie state.
  void clear();

  int open_file_from(const string &name, dentry *relbase, int flags, int mode, inode::filetype type);
  int open_file_from(const string &name, int dirfd, int flags, int mode = 0, inode::filetype type = inode::File);
  int open_file(const string &name, int flags, int mode = 0, inode::filetype type = inode::File);
  int close_file(int fd);

  void send_signal(int sig);

  // Sets heap end. Returns the new end on success, and old end on failure.
  va_t brk(va_t addr);
  
  int nexttid() {
    static spinlock lock;
    synchronized syn(lock);
    return ++tidn;
  }
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

void init(tcb_t *tcb);
void init_user(tcb_t *tcb);
// We can terminate a thread or a process.
void terminate(tcb_t *tcb, int ret);
void terminate(pcb_t *pcb, int ret);

// Set up the returning from the trap handler.
void trap_return_setup(tcb_t *pcb);

// Suspend the current system call.
void suspend();

// Gives the next free pid.
int nextpid();

// Forks a process.
tcb_t *clone(unsigned flags, va_t usp, void *tls);

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

  tcb->status = Init;
  tcb->pc = (va_t) fptr;
  // We aren't lazy-allocating here.
  // Also don't forget that stack grows downwards.
  constexpr auto usp_size = 16_kb - 16;
  tcb->usp = (va_t) vmalloc<16>(usp_size) + usp_size;
  tcb->pcb = pcb;
  tcb->tid = pcb->nexttid();
  pcb->threads.push_back(tcb);
  pidmap->insert(pcb->pid, pcb);
  init(tcb);
  return tcb;
}

void copy_to_user(void *usr, const void *ker, size_t len);

}

#endif
