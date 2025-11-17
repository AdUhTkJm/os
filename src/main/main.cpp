#include <stdint.h>
#include "../utils/libc.h"
#include "../utils/plic.h"
#include "../mem/ptable.h"
#include "../fs/initramfs.h"
#include "../mem/kalloc.h"

static_assert(os::is_same_v<int64_t, long>);
static_assert(os::is_same_v<size_t, unsigned long>);

void kernel_main() {
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  printk("Kernel launched.\n");
  os::init_plic();
  printk("PLIC enabled.\n");
  os::init_pagetable();
  printk("Page table initialized.\n");
  os::init_pm_allocator();
  printk("Allocator initialized.\n");
  os::mount_initramfs();
  for (;;) ;
}
