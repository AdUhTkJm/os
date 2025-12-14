.global sbicall
.global _Z6rdtimev

sbicall:
  ecall
  ret

# long rdtime();
_Z6rdtimev:
  rdtime a0
  ret
