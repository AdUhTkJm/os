#include "initramfs.h"
#include "vfs.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"
#include "../proc/elf.h"

using os::operator""_kb;

namespace {

uint32_t read_int(void *p) {
  return to_big_endian(*(uint32_t *) p);
}

size_t as_int(const char *p) {
  char size[9];
  memcpy(size, p, 8);
  size[8] = '\0';
  return strtoul(size, nullptr, 16);
}

}

namespace os {

size_t initramfs_inode::read(size_t offset, void *buf, size_t len) {
  len = min(size - offset, len);
  memcpy(buf, (char *) data + offset, len);
  return len;
}

// This is read-only.
size_t initramfs_inode::write(size_t, const void *, size_t) {
  return -1ul;
}

void mount_initramfs() {
  char *initrd_start, *initrd_end;
  void *pstart = fdt::query("/chosen", "linux,initrd-start");
  void *pend = fdt::query("/chosen", "linux,initrd-end");
  if (!pstart || !pend)
    panic("device tree: cannot find initrd");
  
  // Read the device tree and find the chosen node.
  initrd_start = (char *) (uintptr_t) read_int(pstart);
  initrd_end = (char *) (uintptr_t) read_int(pend);
  auto map_begin = os::rounddown<4_kb>(initrd_start);
  unsigned size = initrd_end - map_begin;
  for (unsigned i = 0; i < os::roundup<4_kb>(size); i += 4_kb)
    pmap((pa_t) map_begin + i, (va_t) map_begin + i, MAP_4KB, PTE_RWX | PTE_V | PTE_G);
  
  // Initialize the initramfs and register it in vfs.
  vfs_static.construct();
  auto &vfs = *vfs_static;
  inode *root = new initramfs_inode(nullptr, inode::Dir, "/", 0, nullptr);
  vfs.mounts.push_back({ .root = root, .fs_type = "initramfs" });
  
  for (auto *cpio = (cpio_newc_header_t *) initrd_start;;) {
    if (strncmp(cpio->magic, "070701", 6) != 0)
      panic("cpio: magic number not found");

    char *fullpath = (char *) (cpio + 1);
    if (strcmp(fullpath, "TRAILER!!!") == 0)
      break;

    char *data = os::roundup<4>(fullpath + as_int(cpio->namesize));

    inode *cur = root;
    size_t filesize = as_int(cpio->filesize);

    // Skip the initial empty string.
    // Moreover, the root is automatically skipped entirely.
    auto range = split(fullpath, "/");
    for (auto it = ++range.begin(); it != range.end(); ++it) {
      string name = *it;
      inode *k = cur->children[name];
      if (!k) {
        // The file is still unrecorded.
        auto filetype = as_int(cpio->mode) & 0x40000 ? inode::Dir : inode::File;
        cur->add_child(k = new initramfs_inode(cur, filetype, name, filesize, data));
      }
      cur = k;
    }
    cpio = (cpio_newc_header_t *) os::roundup<4>(data + filesize);
  }

  printk("initramfs mounted [%p - %p].\n", initrd_start, initrd_end);
  
  file *init = vfs.open("/init", O_RDONLY);
  if (!init)
    panic("initramfs: cannot find /init");
  load_elf(init);
}

}
