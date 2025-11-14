#include "initramfs.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"

using os::operator""_kb;

namespace {

uint32_t read_int(void *p) {
  return rev_endian(*(uint32_t *) p);
}

}

C void build_initramfs() {
  char *initrd_start, *initrd_end;
  void *pstart = query_fdt("/chosen", "linux,initrd-start");
  void *pend = query_fdt("/chosen", "linux,initrd-end");
  if (!pstart || !pend)
    panic("device tree: cannot find initrd");
  
  // Read the device tree and find the chosen node.
  initrd_start = (char *) (uintptr_t) read_int(pstart);
  initrd_end = (char *) (uintptr_t) read_int(pend);
  printk("initramfs: [%p - %p]\n", initrd_start, initrd_end);
  unsigned size = initrd_end - initrd_start;
  for (unsigned i = 0; i < os::roundup<4_kb>(size); i += 4_kb)
    pmap((pa_t) initrd_start + i, (va_t) initrd_start + i, MAP_4KB, PTE_RWX | PTE_V | PTE_G);
  
  for (int i = 0; i < 6; i++)
    kputch(initrd_start[i]);
}
