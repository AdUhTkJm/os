#include <stdint.h>
#include "../utils/libc.h"
#include "../utils/plic.h"
#include "../mem/ptable.h"
#include "../fs/initramfs.h"

void kernel_main() {
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  printk("Kernel launched.\n");
  init_plic();
  printk("PLIC enabled.\n");
  init_pagetable();
  printk("Page table initialized.\n");
  build_initramfs();
  for (;;) ;
}
