#ifdef __loongarch__

.section .text.low
.globl _start

# This is a translation from RISC-V.

_start:
  csrwr $zero, 0xc # eentry
  lu12i.w $t0, -523776 # 0x8020'0000
  b _Z11kernel_mainv

#endif
