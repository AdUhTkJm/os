#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

#ifdef __cplusplus
#define C extern "C"
#else
#define C
/* Inline behaves slightly different in C and C++. */
/* To make linkage right, we do this hack here. */
#define inline static inline
#endif // #ifdef __cplusplus

/*
Register a6 stands for FID (SBI Function ID), and a7 stands for EID (SBI Extension ID).

See https://github.com/riscv-non-isa/riscv-sbi-doc/releases/tag/v3.0.
*/

#define SBI_DBCN 0x4442434E
#define SBI_DBCN_CONSOLE_WRITE 0, SBI_DBCN
#define SBI_DBCN_CONSOLE_READ 1, SBI_DBCN
#define SBI_DBCN_CONSOLE_WRITE_BYTE 2, SBI_DBCN

#define SBI_TIMER 0x54494D45
#define SBI_SET_TIMER 0, SBI_TIMER

#define SBI_SYSRESET 0x53525354
#define SBI_SYSTEM_RESET 0, SBI_SYSRESET

typedef int64_t reg_t;

typedef struct {
  int err;
  int ret;
} sbiret_t;

C sbiret_t sbicall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a4, reg_t a5, reg_t a6, reg_t a7);

C inline sbiret_t sbi_console_write(reg_t len, const char *s) {
  return sbicall(len, (reg_t) s, 0, 0, 0, 0, SBI_DBCN_CONSOLE_WRITE);
}

C inline sbiret_t sbi_console_write_byte(reg_t byte) {
  return sbicall(byte, 0, 0, 0, 0, 0, SBI_DBCN_CONSOLE_WRITE);
}

C inline sbiret_t sbi_set_timer(reg_t value) {
  return sbicall(value, 0, 0, 0, 0, 0, SBI_SET_TIMER);
}

C [[noreturn]] inline void sbi_system_reset() {
  sbicall(0, 0, 0, 0, 0, 0, SBI_SYSTEM_RESET);
} // NOLINT (Suppress warning of "noreturn")

#ifndef __cplusplus
#undef inline
#endif

#endif
