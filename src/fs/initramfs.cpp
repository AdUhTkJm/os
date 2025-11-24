#include "initramfs.h"
#include "vfs.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"
#include "../proc/elf.h"

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
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, (char *) data + offset, l);
  return l;
}

// This is read-only.
size_t initramfs_inode::write(size_t, const void *, size_t) {
  return 0;
}

// This is read-only.
result initramfs_inode::create(const string &, filetype) {
  return result::failure;
}

initramfs_inode *initramfs_inode::load(const string &name, filetype ty, size_t sz, void *ptr) {
  auto *child = cast<initramfs_inode>(fs->get());
  child->name = name;
  child->type = ty;
  child->size = sz;
  child->data = ptr;
  children[child->name] = child;
  return child;
}

inode *initramfs_inode::lookup(const string &name) {
  return children[name];
}

vector<inode*> initramfs_inode::list() {
  vector<inode*> result;
  result.reserve(children.size());
  for (auto [_, inode] : children)
    result.push_back(inode);
  return result;
}

void mount_initramfs() {
  char *initrd_start, *initrd_end;
  void *pstart = fdt::query("/chosen", "linux,initrd-start");
  void *pend = fdt::query("/chosen", "linux,initrd-end");
  if (!pstart || !pend)
    panic("device tree: cannot find initrd");
  
  // Read the device tree and find the chosen node.
  initrd_start = (char *) as_va(read_int(pstart));
  initrd_end = (char *) as_va(read_int(pend));
  
  // Initialize the initramfs and register it in vfs.
  fs *initramfs = new class initramfs;
  auto *dentry = initramfs->root;
  inode *root = dentry->node;
  vfs.construct(dentry);
  vfs->mount("initram", dentry, dentry);
  
  for (auto *cpio = (cpio_newc_header_t *) initrd_start;;) {
    if (strncmp(cpio->magic, "070701", 6) != 0)
      panic("cpio: magic number not found");

    char *fullpath = (char *) (cpio + 1);
    if (strcmp(fullpath, "TRAILER!!!") == 0)
      break;

    char *data = os::roundup<4>(fullpath + as_int(cpio->namesize));
    printk("loaded: %s\n", fullpath);

    auto *cur = cast<initramfs_inode>(root);
    size_t filesize = as_int(cpio->filesize);

    auto range = split(fullpath, "/");
    for (auto it = range.begin() ; it != range.end(); ++it) {
      string name = *it;
      auto filetype = as_int(cpio->mode) & 0x40000 ? inode::Dir : inode::File;
      cur = cur->load(name, filetype, filesize, data);
    }
    cpio = (cpio_newc_header_t *) os::roundup<4>(data + filesize);
  }

  printk("Mounted initramfs [%p - %p].\n", initrd_start, initrd_end);
}

}
