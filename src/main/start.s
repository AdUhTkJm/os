.section .text
.global _start

_start:
  la sp, stack_top

  # Set up interrupt vectors.
  # On interrupt, the CPU does the following:
  # 1. `medeleg` is checked to know which mode is to handle the exception.
  #    OpenSBI does it for us.
  # 2. CPU saves registers `scause` (interrupt type), `sval` (additional interrupt
  #    type), `sepc` (PC at interrupt), `sstatus` (U/S-mode).
  # 3. Jumps to `stvec`, as we're in S-mode.
  # 4. Returns. `sret` jumps to `sepc`, but normal `ret` jumps to ra.

  # We set stvec to the interrupt handler.
  # la a0, stvec_pos
  # csrw stvec, a0
  j kernel_main

stvec_pos:
  # We must save all registers for a seamless recover.
  # To do this, we preserve the current value of `sp` in `sscratch`.
  csrw sscratch, sp
  addi sp, sp, -124
  sw ra, 0(sp)
  sw gp, 4(sp)
  sw tp, 8(sp)
  sw t0, 12(sp)
  sw t1, 16(sp)
  sw t2, 20(sp)
  sw t3, 24(sp)
  sw t4, 28(sp)
  sw t5, 32(sp)
  sw t6, 36(sp)
  sw a0, 40(sp)
  sw a1, 44(sp)
  sw a2, 48(sp)
  sw a3, 52(sp)
  sw a4, 56(sp)
  sw a5, 60(sp)
  sw a6, 64(sp)
  sw a7, 68(sp)
  sw s0, 72(sp)
  sw s1, 76(sp)
  sw s2, 80(sp)
  sw s3, 84(sp)
  sw s4, 88(sp)
  sw s5, 92(sp)
  sw s6, 96(sp)
  sw s7, 100(sp)
  sw s8, 104(sp)
  sw s9, 108(sp)
  sw s10, 112(sp)
  sw s11, 116(sp)
  csrr a0, sscratch
  sw a0, 120(sp)

  mv a0, sp
  csrr a1, scause
  csrr a2, stval
  csrr a3, sepc
  call interrupt_handler

  lw ra, 0(sp)
  lw gp, 4(sp)
  lw tp, 8(sp)
  lw t0, 12(sp)
  lw t1, 16(sp)
  lw t2, 20(sp)
  lw t3, 24(sp)
  lw t4, 28(sp)
  lw t5, 32(sp)
  lw t6, 36(sp)
  lw a0, 40(sp)
  lw a1, 44(sp)
  lw a2, 48(sp)
  lw a3, 52(sp)
  lw a4, 56(sp)
  lw a5, 60(sp)
  lw a6, 64(sp)
  lw a7, 68(sp)
  lw s0, 72(sp)
  lw s1, 76(sp)
  lw s2, 80(sp)
  lw s3, 84(sp)
  lw s4, 88(sp)
  lw s5, 92(sp)
  lw s6, 96(sp)
  lw s7, 100(sp)
  lw s8, 104(sp)
  lw s9, 108(sp)
  lw s10, 112(sp)
  lw s11, 116(sp)
  lw sp, 120(sp)
  sret

.section .bss
  .space 4096
stack_top:
