.global sbicall
.global rv_rdtime

sbicall:
  ecall
  ret

rv_rdtime:
  rdtime a0
  ret
