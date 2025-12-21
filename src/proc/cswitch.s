#ifdef __riscv

.global _ZN2os12context_saveEPvPb
.global context_restore
.section .text

# int context_save(void *ctx, bool *ctx_valid)
_ZN2os12context_saveEPvPb:
  # Store all necessary registers into ctx.
  sd s0, 0(a0)
  sd s1, 8(a0)
  sd s2, 16(a0)
  sd s3, 24(a0)
  sd s4, 32(a0)
  sd s5, 40(a0)
  sd s6, 48(a0)
  sd s7, 56(a0)
  sd s8, 64(a0)
  sd s9, 72(a0)
  sd s10,80(a0)
  sd s11,88(a0)
  sd ra, 96(a0)
  sd sp, 104(a0)
  csrr t1, sepc
  sd t1, 112(a0)
  csrr t1, sstatus
  sd t1, 120(a0)
  # Mark ctx as valid.
  li t1, 1
  sb t1, 0(a1)

  # Call scheduler_t::dispatch() to switch to the next process.
  # Don't forget the implicit this.
  la a0, _ZN2os9schedulerE
  # We don't use yield() here; the thread's status should have changed
  # by prepare_sleep() (or in wait_queue::prepare) beforehand.
  call _ZN2os11scheduler_t8dispatchEv # noreturn

# extern "C" [[noreturn]] void context_save(void *ctx, bool from_signal)
context_restore:
  ld s0, 0(a0)
  ld s1, 8(a0)
  ld s2, 16(a0)
  ld s3, 24(a0)
  ld s4, 32(a0)
  ld s5, 40(a0)
  ld s6, 48(a0)
  ld s7, 56(a0)
  ld s8, 64(a0)
  ld s9, 72(a0)
  ld s10,80(a0)
  ld s11,88(a0)
  ld ra, 96(a0)
  ld sp, 104(a0)
  ld a2, 112(a0)
  csrw sepc, a2
  ld a2, 120(a0)
  csrw sstatus, a2
2:
  # a0 = a1 ? -EINTR : 0
  bnez a1, 3f
  li a0, 0
  jr ra
3:
  li a0, -4
  jr ra

#endif

