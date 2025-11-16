.global sbicall
.global _Z9rv_rdtimev

sbicall:
  ecall
  ret

# long rv_rdtime();
_Z9rv_rdtimev:
  rdtime a0
  ret
