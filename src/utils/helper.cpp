#include "sbi.h"
#include "libc.h"
#include "helper.h"
#include "../mem/ptable.h"

C void kputs(const char *s) {
  unsigned len = strlen(s);
  auto *pa = (const char *) os::to_pa((va_t) s);
  [[unlikely]] if ((pa_t) pa == -1ul)
    panic("kputs failed");
  sbi_console_write(len, pa);
}

C void kputch(char c) {
  sbi_console_write(1, &c);
}

C void panic(const char *s) {
  printk("kernel panicked: %s\n", s);
  sbi_system_reset();
}

C uint32_t to_big_endian(uint32_t x) {
  unsigned byte0 = x & 0xff;
  unsigned byte1 = (x >> 8) & 0xff;
  unsigned byte2 = (x >> 16) & 0xff;
  unsigned byte3 = (x >> 24) & 0xff;
  return byte3 + (byte2 << 8) + (byte1 << 16) + (byte0 << 24);
}

C uint64_t rev_endian64(uint64_t x) {
  uint64_t byte0 = x & 0xff;
  uint64_t byte1 = (x >> 8) & 0xff;
  uint64_t byte2 = (x >> 16) & 0xff;
  uint64_t byte3 = (x >> 24) & 0xff;
  uint64_t byte4 = (x >> 32) & 0xff;
  uint64_t byte5 = (x >> 40) & 0xff;
  uint64_t byte6 = (x >> 48) & 0xff;
  uint64_t byte7 = (x >> 56) & 0xff;
  return byte7 + (byte6 << 8) + (byte5 << 16) + (byte4 << 24)
    + (byte3 << 32) + (byte2 << 40) + (byte1 << 48) + (byte0 << 56);
}
