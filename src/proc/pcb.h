#ifndef PCB_H
#define PCB_H

#include "../utils/helper.h"
#include "../mem/ptable.h"
#include "vma.h"

namespace os {

typedef enum {
  PROC_RUNNING,
  PROC_SUSPEND,
  PROC_READY,
} process_status_t;

typedef struct {
  reg_t ra;
  reg_t gp;
  reg_t tp;
  reg_t t0;
  reg_t t1;
  reg_t t2;
  reg_t t3;
  reg_t t4;
  reg_t t5;
  reg_t t6;
  reg_t a0;
  reg_t a1;
  reg_t a2;
  reg_t a3;
  reg_t a4;
  reg_t a5;
  reg_t a6;
  reg_t a7;
  reg_t s0;
  reg_t s1;
  reg_t s2;
  reg_t s3;
  reg_t s4;
  reg_t s5;
  reg_t s6;
  reg_t s7;
  reg_t s8;
  reg_t s9;
  reg_t s10;
  reg_t s11;
  reg_t sp;
} regframe_t;

#ifdef __cplusplus
static_assert(sizeof(regframe_t) == 31 * 8);
#endif

struct pcb {
  int pid;
  process_status_t status;
  regframe_t *frame;
  pa_t pt_root;
  os::vector<vma> vma;
};

}

#endif
