.global sbicall
.global _Z6rdtimev

#ifdef __riscv
sbicall:
  ecall
  ret

# long rdtime();
_Z6rdtimev:
  rdtime a0
  ret
#endif

#ifdef __loongarch__
_Z6rdtimev:
  rdtime.d $a0
  ret
#endif
