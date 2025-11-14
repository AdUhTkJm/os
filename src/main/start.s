.section .text
.global _start

_start:
  la sp, __stack_top

  # After OpenSBI finishes setup, a0 will be hart ID and a1 will be
  # the location of flattened device tree (FDT).
  # Now we read the FDT.
  call read_fdt

  # Set up interrupt vectors.
  # On interrupt, the CPU does the following:
  # 1. `medeleg` is checked to know which mode is to handle the exception.
  #    OpenSBI does it for us.
  # 2. CPU saves registers `scause` (interrupt type), `sval` (additional interrupt
  #    type), `sepc` (PC at interrupt), `sstatus` (U/S-mode).
  # 3. Jumps to `stvec`, as we're in S-mode.
  # 4. Returns. `sret` jumps to `sepc`, but normal `ret` jumps to ra.

  # We set stvec to the interrupt handler.
  la a0, stvec_pos
  csrw stvec, a0

  # `sie` bit 9 masks interrupts when unset. Note bit starts from 0.
  # `sstatus` bit 1 enables/disables all interrupt in S-mode.
  # See https://riscv.github.io/riscv-isa-manual/snapshot/privileged/#sstatus
  li a0, 512
  csrs sie, a0
  csrsi sstatus, 2

  # We set up the timer.
  # Enable the timer interrupt (bit 5) in sie.
  li a0, 32
  csrs sie, a0
  rdtime a0
  li a1, 5000000
  add a0, a0, a1
  li a7, 0x54494D45 # "TIMER"
  li a6, 0 # set_timer
  ecall

  j kernel_main

.align 4
stvec_pos:
  # We must save all registers for a seamless recover.
  # To do this, we preserve the current value of `sp` in `sscratch`.
  csrw sscratch, sp
  addi sp, sp, -128
  sd ra, 0(sp)
  sd gp, 8(sp)
  sd tp, 16(sp)
  sd t0, 24(sp)
  sd t1, 32(sp)
  sd t2, 40(sp)
  sd t3, 48(sp)
  sd t4, 56(sp)
  sd t5, 64(sp)
  sd t6, 72(sp)
  sd a0, 80(sp)
  sd a1, 88(sp)
  sd a2, 96(sp)
  sd a3, 104(sp)
  sd a4, 112(sp)
  sd a5, 120(sp)
  sd a6, 128(sp)
  sd a7, 136(sp)
  sd s0, 144(sp)
  sd s1, 152(sp)
  sd s2, 160(sp)
  sd s3, 168(sp)
  sd s4, 176(sp)
  sd s5, 184(sp)
  sd s6, 192(sp)
  sd s7, 200(sp)
  sd s8, 208(sp)
  sd s9, 216(sp)
  sd s10, 224(sp)
  sd s11, 232(sp)
  csrr a0, sscratch
  sd a0, 240(sp)

  mv a0, sp
  csrr a1, scause
  csrr a2, stval
  csrr a3, sepc
  call interrupt_handler

  ld ra, 0(sp)
  ld gp, 8(sp)
  ld tp, 16(sp)
  ld t0, 24(sp)
  ld t1, 32(sp)
  ld t2, 40(sp)
  ld t3, 48(sp)
  ld t4, 56(sp)
  ld t5, 64(sp)
  ld t6, 72(sp)
  ld a0, 80(sp)
  ld a1, 88(sp)
  ld a2, 96(sp)
  ld a3, 104(sp)
  ld a4, 112(sp)
  ld a5, 120(sp)
  ld a6, 128(sp)
  ld a7, 136(sp)
  ld s0, 144(sp)
  ld s1, 152(sp)
  ld s2, 160(sp)
  ld s3, 168(sp)
  ld s4, 176(sp)
  ld s5, 184(sp)
  ld s6, 192(sp)
  ld s7, 200(sp)
  ld s8, 208(sp)
  ld s9, 216(sp)
  ld s10, 224(sp)
  ld s11, 232(sp)
  ld sp, 240(sp)
  sret
