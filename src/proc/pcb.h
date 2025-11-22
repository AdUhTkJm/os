#ifndef PCB_H
#define PCB_H

#include "../utils/helper.h"
#include "../mem/ptable.h"
#include "../mem/vma.h"
#include "../mem/kalloc.h"

namespace os {

// Near the highest address in the lower-half space.
constexpr va_t stack_top = 0x3f'f000'0000ul;
constexpr size_t user_stack_size = 8_mb;
constexpr size_t kstack_size = 8_kb;

enum process_state {
  Init, Running, Sleeping, Ready, Zombie, Dead
};

struct trapframe {
  reg_t regs[30]; // zero and sp are not stored.
  reg_t sepc;
  reg_t sstatus;
};

#ifdef __cplusplus
static_assert(sizeof(trapframe) == 256);
#endif

class process_file_table {
  os::hashmap<int, file*> open;
public:
  file *operator[](int x) { return open.count(x) ? open[x] : nullptr; }
  int allocate(file *f);
  void deallocate(int fd);
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
  process_file_table ftbl;// Process file table.

  ~pcb_t() {
    pfree(pt_root);
    vfree(1 + (trapframe *) ksp);
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

void init(pcb_t *pcb);
void terminate(pcb_t *pcb, int ret);

// Set up the returning from the trap handler.
void trap_return_setup(pcb_t *pcb);

}

#endif
