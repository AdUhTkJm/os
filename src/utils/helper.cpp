#include "sbi.h"
#include "libc.h"
#include "helper.h"
#include "../mem/ptable.h"

C void kputs(const char *s) {
  unsigned len = strlen(s);
  auto *pa = (const char *) os::to_pa(s);
  sbi_console_write(len, pa);
}

C void kputch(char c) {
  sbi_console_write(1, (const char*) os::to_pa(&c));
}

C void panic(const char *s) {
  printk("kernel panicked: %s\n", s);
  sbi_system_reset();
}

C void hexdump(const void *ptr, size_t len) {
  const uint8_t *buf = (const uint8_t *)ptr;
  for (size_t i = 0; i < len; i += 16) {
    // Print physical address instead.
    printk("%08p  ", uintptr_t(ptr) + i);

    for (size_t j = 0; j < 16; ++j) {
      if (i + j < len)
        printk("%02x ", buf[i + j]);
      else
        printk("   ");
    }

    printk(" ");

    for (size_t j = 0; j < 16 && i + j < len; ++j) {
      uint8_t c = buf[i + j];
      printk("%c", (c >= 32 && c < 127) ? c : '.');
    }

    printk("\n");
  }
}
