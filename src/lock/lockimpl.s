.section .text
.global _ZN2os8spinlock12release_implEPi
.global _ZN2os8spinlock12acquire_implEPi

#ifdef __riscv
_ZN2os8spinlock12acquire_implEPi:
  li a1, 1
1:
  amoswap.w.aq a1, a1, (a0)
  bnez a1, 1b
  ret

_ZN2os8spinlock12release_implEPi:
  amoswap.w.rl zero, zero, (a0)
  ret
#endif

#ifdef __loongarch__
_ZN2os8spinlock12acquire_implEPi:
  ll.w $a1, $a0, 0
  bnez $a1, _ZN2os8spinlock12acquire_implEPi
  addi.w $a1, $zero, 1
  sc.w $a1, $a0, 0
  beqz $a1, _ZN2os8spinlock12acquire_implEPi
  dbar 0
  ret

_ZN2os8spinlock12release_implEPi:
  addi.w $a1, $zero, 0
  st.w $a1, $a0, 0
  dbar 0
  ret

#endif
