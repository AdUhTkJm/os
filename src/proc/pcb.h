#ifndef PCB_H
#define PCB_H

#include "../utils/helper.h"
#include "../mem/ptable.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"
#include "../utils/stl/optional.h"

namespace os {

// Near the highest address in the lower-half space.
constexpr va_t stack_top = 0xf'f000'0000ul;
constexpr size_t user_stack_size = 8_mb;
constexpr size_t kstack_size = 8_kb;

enum process_state {
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

class process_file_table {
public:
  using fddesc = unsigned char;
private:
  // The open file table.
  os::hashmap<int, file*> open;
  // The file descriptor table.
  os::hashmap<int, fddesc> desc;
public:
  int allocate(file *f, int fd = -1);
  void deallocate(int fd);
  void clear();
  int count(int fd) { return open.count(fd); }
  
  file *&operator[](int x) { return open[x]; }
  void set_desc(int fd, fddesc desc) { this->desc[fd] = desc; }
  optional<fddesc> get_desc(int fd) { return desc.count(fd) ? optional(desc[fd]) : nullopt; }
  
  using iterator = decltype(open)::iterator;
  iterator begin() { return open.begin(); }
  iterator end() { return open.end(); }
};

struct pcb_t : os::intrusive_list_node<pcb_t> {
  int pid;                // Process id.
  process_state status;   // Process status (running, sleeping etc.)
  pa_t pt_root;           // Root page table entry.
  va_t ksp;               // Kernel stack for this process.
  va_t usp;               // User stack top for this process.
  va_t pc;                // Program counter.
  os::vector<vma_t> vma;  // VMAs.
  pcb_t *parent;          // Parent.
  int ret;                // Return value.
  bool ctx_valid = false; // Whether the syscall progress is valid. See below.
  bool kproc = false;     // Whether this is a kernel process.
  process_file_table ftbl;// Process file table.
  ctxframe ctx;           // System call progress, for resuming blocking syscalls.
  os::intrusive_list<pcb_t> children;
  int uid, gid, euid, suid;

  // Note this is not the destructor. PCB will need to release its resources
  // before destruction, and then put itself to a zombie state.
  void clear();
  void clear_vma();
  int open_file(const string &name, int flags, int mode = 0);
  int close_file(int fd);

  // Sets heap end. Returns the new end on success, and old end on failure.
  va_t brk(va_t addr);

  // This is the final deletion. This cannot be done in clear(), because it
  // is called when this ksp is in use.
  ~pcb_t() {
    auto ksp_bottom = ksp + sizeof(trapframe) - kstack_size;
    vfree((void*) ksp_bottom);
  }
};
/*
Note for ksp:
  It actually does not point to top of the kernel stack.
  It points to the place after trap frame, i.e.

  [ra tp gp ... t6 sepc sstatus | <stack>]
                                |----------- ksp
*/

static_assert(offsetof(pcb_t, ksp) == 32);

extern static_storage<hashmap<int, pcb_t*>> pid_map_s;

void init(pcb_t *pcb);
void init_user(pcb_t *pcb);
void terminate(pcb_t *pcb, int ret);

// Set up the returning from the trap handler.
void trap_return_setup(pcb_t *pcb);

// Suspend the current system call.
void suspend();

// Gives the next free pid.
int nextpid();

// Forks a process.
int fork();

// Replaces a process image.
int exec(const string &path, char *const *argv, char *const *envp);

// Even though it does not return for now, it will eventually look as if it "returned".
extern "C" void context_save(void *ctx, bool *ctx_valid);
extern "C" [[noreturn]] void context_restore(void *ctx);

#define suspend() context_save(&scheduler.active->ctx, &scheduler.active->ctx_valid)

// Creates a kernel process.
template<class T> requires (is_function_v<remove_pointer_t<T>>)
pcb_t *make_kprocess(T fptr) {
  pcb_t *pcb = new pcb_t;
  pcb->status = Init;
  pcb->pc = (va_t) fptr;
  pcb->pid = nextpid();
  pcb->kproc = true;
  pcb->gid = pcb->uid = 0; // root
  // We aren't lazy-allocating here.
  pcb->usp = (va_t) vmalloc<16>(16_kb);
  init(pcb);
  return pcb;
}

void copy_to_user(void *usr, const void *ker, size_t len);

}

#endif
