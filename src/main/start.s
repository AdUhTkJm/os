#ifdef __riscv

.section .text.low
.global _start

_start:
  csrw stvec, zero
  li t0, 1074794496
  slli t0, t0, 1 # 0x80202000
  # Store information for FDT.
  sd a0, 8(t0)
  sd a1, 16(t0)
  j _Z11kernel_mainv

.section .text.high
_start_high:
  la sp, __stack_top

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
  j _Z9main_highv

.section .text
.global __handler_end
.align 4
stvec_pos:
  # Record current sp.
  csrw sscratch, sp
  
  # Load the ksp of the process.
  la sp, _ZN2os9schedulerE # os::scheduler
  ld sp, 48(sp)            # scheduler_t::active
  ld sp, 24(sp)            # tcb_t::ksp

  # Save registers to the kernel stack.
  sd ra, 0(sp)
  sd gp, 8(sp)
  sd tp, 16(sp)
  sd t0, 24(sp)
  sd t1, 32(sp)
  sd t2, 40(sp)
  sd s0, 48(sp)
  sd s1, 56(sp)
  sd a0, 64(sp)
  sd a1, 72(sp)
  sd a2, 80(sp)
  sd a3, 88(sp)
  sd a4, 96(sp)
  sd a5, 104(sp)
  sd a6, 112(sp)
  sd a7, 120(sp)
  sd s2, 128(sp)
  sd s3, 136(sp)
  sd s4, 144(sp)
  sd s5, 152(sp)
  sd s6, 160(sp)
  sd s7, 168(sp)
  sd s8, 176(sp)
  sd s9, 184(sp)
  sd s10,192(sp)
  sd s11,200(sp)
  sd t3, 208(sp)
  sd t4, 216(sp)
  sd t5, 224(sp)
  sd t6, 232(sp)
  csrr t0, sscratch
  sd t0, 240(sp)
  csrr a2, sepc
  sd a2, 248(sp)
  csrr t0, sstatus
  sd t0, 256(sp)
  csrr a0, scause
  csrr a1, stval

  call _ZN2os17interrupt_handlerEllPv

__handler_end:
  call _ZN2os9sighandleEv

  # Load the ksp of the current process.
  # (The process might have changed.)
  la t0, _ZN2os9schedulerE
  ld t1, 48(t0)
  ld sp, 24(t1)

  # Restore the registers.
  ld t0, 256(sp)
  csrw sstatus, t0
  ld t0, 248(sp)
  csrw sepc, t0
  ld ra, 0(sp)
  ld gp, 8(sp)
  ld tp, 16(sp)
  ld t0, 24(sp)
  ld t1, 32(sp)
  ld t2, 40(sp)
  ld s0, 48(sp)
  ld s1, 56(sp)
  ld a0, 64(sp)
  ld a1, 72(sp)
  ld a2, 80(sp)
  ld a3, 88(sp)
  ld a4, 96(sp)
  ld a5, 104(sp)
  ld a6, 112(sp)
  ld a7, 120(sp)
  ld s2, 128(sp)
  ld s3, 136(sp)
  ld s4, 144(sp)
  ld s5, 152(sp)
  ld s6, 160(sp)
  ld s7, 168(sp)
  ld s8, 176(sp)
  ld s9, 184(sp)
  ld s10,192(sp)
  ld s11,200(sp)
  ld t3, 208(sp)
  ld t4, 216(sp)
  ld t5, 224(sp)
  ld t6, 232(sp)
  ld sp, 240(sp)
  sret

#endif

#ifdef __loongarch__

.section .text.low
.globl stvec_pos

# Unlike RISC-V, we do not need a separate _start,
# as we don't really need to store a0 (FDT address) and a1 (hart id).
# The linker will place kernel_main() at the correct place.

stvec_pos:

.section .text.high
.globl _start_high
_start_high:
  b _Z9main_highv

#endif
