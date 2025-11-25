using reg_t = long;

[[gnu::naked]]
int syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a4, reg_t a5, reg_t a6, reg_t a7) {
  __asm__ volatile(
    "ecall\n"
    "ret\n"
  ::: "memory");
}

extern "C" void _start() {
  __asm__ volatile(
    "li a7, 60\n"
    "ecall\n"
  );
  syscall(1, (reg_t) "Hello World!", 13, 0, 0, 0, 0, 1);
  syscall(0, 0, 0, 0, 0, 0, 0, 60);
}
