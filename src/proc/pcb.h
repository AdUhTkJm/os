#ifndef PCB_H
#define PCB_H

#include "../utils/helper.h"
#include "../mem/ptable.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"

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

#ifdef __cplusplus
static_assert(sizeof(trapframe) == 272);
#endif

class process_file_table {
  os::hashmap<int, file*> open;
public:
  int allocate(file *f, int fd = -1);
  void deallocate(int fd);
  ~process_file_table() {
    for (auto [_, f] : open) {
      if (!--f->refcnt)
        delete f;
    }
  }
  
  file *&operator[](int x) { return open[x]; }
  
  using iterator = decltype(open)::iterator;
  iterator begin() { return open.begin(); }
  iterator end() { return open.end(); }
};

union syscall_progress {
  struct read {
    int cur; // Current offset from buf.
  } read;
};

struct pcb_t : os::intrusive_list_node<pcb_t> {
  int pid;                // Process id.
  process_state status;   // Process status (running, sleeping etc.)
  pa_t pt_root;           // Root page table entry.
  va_t ksp;               // Kernel stack for this process.
  va_t pc;                // Program counter.
  os::vector<vma_t> vma;  // VMAs.
  pcb_t *parent;          // Parent.
  int ret;                // Return value.
  bool prog_valid;        // Whether the syscall progress is valid. See below.
  process_file_table ftbl;// Process file table.
  syscall_progress prog;  // System call progress, for resuming blocking syscalls.

  ~pcb_t() {
    pt::free(pt_root);
    vfree(1 + (trapframe *) ksp);
  }
  void open_file(const string &name);
  void close_file(const string &name);
};
/*
Note for ksp:
  It actually does not point to top of the kernel stack.
  It points to the place after trap frame, i.e.

  [ra tp gp ... t6 sepc sstatus | <stack>]
                                |----------- ksp
*/

static_assert(offsetof(pcb_t, ksp) == 32);

void init(pcb_t *pcb);
void terminate(pcb_t *pcb, int ret);

// Set up the returning from the trap handler.
void trap_return_setup(pcb_t *pcb);

// Gives the next free pid.
int nextpid();

// Forks a process.
int fork();

}

#endif
