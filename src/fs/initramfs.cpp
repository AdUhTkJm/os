#include "initramfs.h"
#include "vfs.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"
#include "../proc/elf.h"
#include "../proc/schedule.h"

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

class initramfs *initramfs;

ssize_t initramfs_inode::read(size_t offset, void *buf, size_t len, int) {
  ssize_t l = min(long(sz) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, (char *) data + offset, l);
  return l;
}

// This is read-only.
ssize_t initramfs_inode::write(size_t, const void *, size_t, int) {
  return 0;
}

initramfs_inode *initramfs_inode::load(const string &name, filetype ty, size_t sz, void *ptr) {
  auto *child = cast<initramfs_inode>(fs->get());
  child->type = ty;
  child->sz = sz;
  child->data = ptr;
  children[name] = child;
  child->linked();
  return child;
}

inode *initramfs_inode::lookup(const string &name) {
  if (!children.count(name))
    return nullptr;
  return children[name];
}

vector<inode::item> initramfs_inode::list() {
  vector<item> result;
  result.reserve(children.size());
  for (auto [name, inode] : children)
    result.push_back({ .inum = (long) inode, .name = name, .ty = inode->type });
  return result;
}

void mount_initramfs() {
  char *initrd_start;
  void *pstart = fdt::query("/chosen", "linux,initrd-start");
  void *pend = fdt::query("/chosen", "linux,initrd-end");
  if (!pstart || !pend)
    panic("device tree: cannot find initrd");
  
  // Read the device tree and find the chosen node.
  initrd_start = (char *) as_va((read_int(pstart) * 1ul << 32) + read_int((char*) pstart + 4));
  
  // Initialize the initramfs and register it in vfs.
  initramfs = new (os::permanent) class initramfs;
  auto *dentry = initramfs->root;
  inode *root = dentry->node;
  auto tcb = active();
  auto pcb = tcb->pcb;
  
  pcb->vfs = new class vfs;
  pcb->vfs->ref();
  vfs::mount(dentry, dentry);
  pcb->vfs->base = dentry;
  dentry->belong->parent = dentry->belong;
  
  for (auto *cpio = (cpio_newc_header_t *) initrd_start;;) {
    if (strncmp(cpio->magic, "070701", 6) != 0)
      panic("cpio: magic number not found");

    char *fullpath = (char *) (cpio + 1);
    if (strcmp(fullpath, "TRAILER!!!") == 0)
      break;

    char *data = os::roundup<4>(fullpath + as_int(cpio->namesize));

    auto *cur = cast<initramfs_inode>(root);
    size_t filesize = as_int(cpio->filesize);

    auto range = split(fullpath, "/");
    for (auto it = range.begin() ; it != range.end(); ++it) {
      string name = *it;
      auto filetype = as_int(cpio->mode) & 040000 /*Octal*/ ? inode::Dir : inode::File;
      cur = cur->load(name, filetype, filesize, data);
    }
    cpio = (cpio_newc_header_t *) os::roundup<4>(data + filesize);
  }
}

}
