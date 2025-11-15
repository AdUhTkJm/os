#include "initramfs.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"

using os::operator""_kb;

namespace {

uint32_t read_int(void *p) {
  return rev_endian(*(uint32_t *) p);
}

// Note that we can't allow global constructor/destructors,
// because __dso_handle and __cxa_atexit in libgcc is not present.
os::static_storage<os::hashmap<const char *, cpio_newc_header_t>> cpio_fs;

size_t as_int(const char *p) {
  char size[9];
  memcpy(size, p, 8);
  size[8] = '\0';
  return strtoul(size, nullptr, 16);
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
  
  cpio_fs.construct();
  auto &files = *cpio_fs;
  
  for (auto *cpio = (cpio_newc_header_t *) initrd_start;;) {
    if (strncmp(cpio->magic, "070701", 6) != 0)
      panic("cpio: magic number not found");

    char *name = (char *) (cpio + 1);
    if (strcmp(name, "TRAILER!!!") == 0)
      break;
    
    files[name] = *cpio;
    name = os::roundup<4>(name + as_int(cpio->namesize));
    name = os::roundup<4>(name + as_int(cpio->filesize));
    cpio = (cpio_newc_header_t *) name;
  }

  printk("files count: %d\n", files.size());
  for (const auto &[x, header] : files) {
    printk("name: %s, filesize: %ld bytes\n", x, as_int(header.filesize));
  }
}
