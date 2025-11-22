.section .text
.global _ZN2os8spinlock12release_implEPi
.global _ZN2os8spinlock12acquire_implEPi

_ZN2os8spinlock12acquire_implEPi:
  li a1, 1
1:
  amoswap.w.aq a1, a1, (a0)
  bnez a1, 1b
  ret

_ZN2os8spinlock12release_implEPi:
  amoand.w.rl zero, zero, (a0)
  ret
