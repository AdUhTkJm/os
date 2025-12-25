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
  size_t sz;

  initramfs_inode *load(const string &name, filetype ty, size_t sz, void *ptr);
  friend void mount_initramfs();
  friend class initramfs;
public:
  using inode_impl::inode_impl;

  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t offset, const void *buf, size_t len, int flags) override;
  int truncate(size_t) override { return -EACCES; }
  int create(const string &name, filetype ty, int) override;
  int unlink(const string &) override { return -EACCES; }
  inode *lookup(const string &name) override;
  vector<item> list() override;
  // We don't support symlinks for now.
  optional<string> readlink() override { return nullopt; }
  // TODO: In fact we should read from disk image.
  meta get_meta() override { return meta(0, 0, 0); }
  void set_meta(const meta &) override {}

  size_t size() const override { return sz; }
  long inum() const override { return (long) data; }
};

class initramfs : public fs {
public:
  initramfs() {
    auto rootnode = new (os::permanent) initramfs_inode(this, 0, 0, 0777, inode::Dir);
    rootnode->sz = 0;
    root = new class dentry("", rootnode, nullptr);
  }

  initramfs_inode *get() override { return new (os::permanent) initramfs_inode(this, 0, 0, 0777, inode::Bad); }
  void erase(inode *) override { }
  bool has_backup() override { return false; }
} extern *initramfs;

}

#endif
