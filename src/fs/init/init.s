.globl _start
.text

_start:
  # write(fd, buf, count)
  li a7, 64
  li a0, 1
  la a1, message
  li a2, 13
  ecall

  # exit(code)
  li a7, 93
  li a0, 0
  ecall

.data
message:
  .string "Hello World!\n"
