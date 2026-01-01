.global _ZN2os12context_saveEPvPb
.global context_restore
.section .text
#ifdef __riscv
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

  # Check deadlock - if we disabled preemption when we suspend(),
  # then it's wrong.
#ifdef DEADLOCK
  call _ZN2os13check_suspendEv
#endif

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

#ifdef __loongarch__
# int context_save(void *ctx, bool *ctx_valid)
# Note that Loongarch has 9 temporaries (2 more than RISC-V) and hence 10 callee-saved ones (2 less).
# We don't try to save space here.
_ZN2os12context_saveEPvPb:
  st.d $s0, $a0, 0
  st.d $s1, $a0, 8
  st.d $s2, $a0, 16
  st.d $s3, $a0, 24
  st.d $s4, $a0, 32
  st.d $s5, $a0, 40
  st.d $s6, $a0, 48
  st.d $s7, $a0, 56
  st.d $s8, $a0, 64
  st.d $s9, $a0, 72
  st.d $ra, $a0, 96
  st.d $sp, $a0, 104
  csrrd $t0, 6 # ERA
  st.d $t0, $a0, 112
  csrrd $t0, 1  # PRMD
  st.d $t0, $a0, 120

  # Mark ctx as valid.
  li.w $t0, 1
  st.b $t0, $a1, 0

  # scheduler.dispatch()
  la.local $a0, _ZN2os9schedulerE
  bl _ZN2os11scheduler_t8dispatchEv  # noreturn

context_restore:
  ld.d $s0, $a0, 0
  ld.d $s1, $a0, 8
  ld.d $s2, $a0, 16
  ld.d $s3, $a0, 24
  ld.d $s4, $a0, 32
  ld.d $s5, $a0, 40
  ld.d $s6, $a0, 48
  ld.d $s7, $a0, 56
  ld.d $s8, $a0, 64
  ld.d $s9, $a0, 72
  ld.d $ra, $a0, 96
  ld.d $sp, $a0, 104
  ld.d $t0, $a0, 112
  csrwr $t0, 6 # ERA
  ld.d $t0, $a0, 120
  csrwr $t0, 1 # PRMD

2:
  # return a1 ? -EINTR : 0
  bnez $a1, 3f
  li.w $a0, 0
  ret
3:
  li.w $a0, -4
  ret
#endif
