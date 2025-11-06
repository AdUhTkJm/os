#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

/* Debug Console */
#define SBI_DBCN 0x4442434E
#define SBI_DBCN_CONSOLE_WRITE 0, SBI_DBCN
#define SBI_DBCN_CONSOLE_READ 1, SBI_DBCN
#define SBI_DBCN_CONSOLE_WRITE_BYTE 2, SBI_DBCN

typedef int64_t reg_t;

typedef struct {
  int err;
  int ret;
} sbiret_t;

/*
Register a6 stands for FID (SBI Function ID), and a7 stands for EID (SBI Extension ID).

See https://github.com/riscv-non-isa/riscv-sbi-doc/releases/tag/v3.0.
*/
extern sbiret_t sbicall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a4, reg_t a5, reg_t a6, reg_t a7);

#endif
