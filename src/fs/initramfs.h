#ifndef INITRAMFS_H
#define INITRAMFS_H

#include "../utils/helper.h"
#include "vfs.h"

/*
For CPIO format, refer to https://man.archlinux.org/man/cpio.5.en.
*/
namespace os {

struct cpio_newc_header_t {
  char magic[6];
  char ino[8];
  char mode[8];
  char uid[8];
  char gid[8];
  char nlink[8];
  char mtime[8];
  char filesize[8];
  char devmajor[8];
  char devminor[8];
  char rdevmajor[8];
  char rdevminor[8];
  char namesize[8];
  char check[8];
};

static_assert(sizeof(cpio_newc_header_t) == 110);

void mount_initramfs();

class initramfs_inode : public inode_impl<initramfs_inode> {
  void *data;
  os::hashmap<string, inode *> children;

  initramfs_inode *load(const string &name, filetype ty, size_t sz, void *ptr);
  friend void mount_initramfs();
public:
  using inode_impl::inode_impl;

  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t offset, const void *buf, size_t len, int flags) override;
  result create(const string &name, filetype ty) override;
  inode *lookup(const string &name) override;
  vector<inode*> list() override;
};

class initramfs : public fs {
public:
  initramfs() {
    auto rootnode = new initramfs_inode(this, 0, 0);
    rootnode->type = inode::Dir;
    rootnode->name = "";
    rootnode->size = 0;
    root = new class dentry("", rootnode);
  }

  initramfs_inode *get() override { return new initramfs_inode(this, 0, 0); }
  void erase(inode *) override { }
  bool has_backup() override { return false; }
};

}

#endif
