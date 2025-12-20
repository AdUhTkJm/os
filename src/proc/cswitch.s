#ifdef __riscv

.global _ZN2os12context_saveEPvPbPNS_8spinlockE
.global _ZN2os12context_saveEPvPbPNS_5mutexE
.global context_restore
.section .text

# int context_save(void *ctx, bool *ctx_valid, spinlock *lock)
_ZN2os12context_saveEPvPbPNS_8spinlockE:
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
  # Store the spinlock.
  li a3, 0
  sw a3, 128(a0)
  sd a2, 136(a0)
  # Mark ctx as valid.
  li t1, 1
  sb t1, 0(a1)

  # Release lock.
#ifdef DEADLOCK
  # The release() is not inlined.
  # The function both handles deadlock record and re-enables interrupt.
  mv a0, a2
  call _ZN2os8spinlock7releaseEv
#else
  # The acquire() might be inlined.
#ifndef UNIPROCESSOR
  mv a0, a2
  call _ZN2os8spinlock12release_implEPi
  # Enable interrupts.
  csrs sie, 2
#endif
#endif

  # Call scheduler_t::yield() to select and switch to the next process.
  # Don't forget the implicit this.
  la a0, _ZN2os9schedulerE
  call _ZN2os11scheduler_t5yieldEb # noreturn

# int context_save(void *ctx, bool *ctx_valid, mutex *lock)
# Similar to the one above, just unlocking changes.
_ZN2os12context_saveEPvPbPNS_5mutexE:
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
  # Store the lock.
  li a3, 1
  sw a3, 128(a0)
  sd a2, 136(a0)
  # Mark the context valid.
  li t0, 1
  sb t0, 0(a1)
  # Release lock. This time we just call release().
  mv a0, a2
  call _ZN2os5mutex7releaseEv
  csrs sie, 2

  la a0, _ZN2os9schedulerE
  call _ZN2os11scheduler_t5yieldEb # noreturn

# extern "C" [[noreturn]] void context_save(void *ctx, bool from_signal)
context_restore:
  # We must preserve this across function call.
  addi sp, sp, -16
  sd a0, 0(sp)
  sd a1, 8(sp)

  # Restore the lock.
  lw a2, 128(a0)
  ld a3, 136(a0)
  bnez a2, 1f
  # This is a spinlock.
#ifdef DEADLOCK
  # The acquire() is not inlined.
  mv a0, a3
  call _ZN2os8spinlock7acquireEv
#else
  # The acquire() might be inlined.
#ifndef UNIPROCESSOR
  mv a0, a3
  call _ZN2os8spinlock12acquire_implEPi
  # Disable interrupts.
  csrc sie, 2
#endif
#endif
  j 2f
1:
  # This is a mutex.
  mv a0, a3
  call _ZN2os5mutex7acquireEv
2:
  # Read the registers from ctx.
  # The ctx_valid is handled outside this function.
  ld a0, 0(sp)
  ld a1, 8(sp)
  
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

