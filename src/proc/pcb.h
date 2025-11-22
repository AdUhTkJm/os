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
  Init, Running, Sleeping, Ready
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

  ~pcb_t() {
    pfree(pt_root);
    vfree((void *) ksp);
  }
};

static_assert(offsetof(pcb_t, ksp) == 32);

void init(pcb_t *pcb);
void destruct(pcb_t *pcb);

// Run the process for the first time.
void activate(pcb_t *pcb, int argc, char **argv, char **envp);
// Switch to the process.
void switch_to(pcb_t *pcb);

}

#endif
