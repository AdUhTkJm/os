#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"
#include "../proc/pcb.h"
#include "../utils/stl/ring_buffer.h"

namespace os {

struct /*interface*/ block_device {
  virtual int read(size_t lba, void *buf) = 0;
  virtual int write(size_t lba, const void *buf) = 0;
};

class devfs : public fs {
public:
  devfs();
  inode *get() override { return nullptr; }
  void erase(inode *) override {}
  bool has_backup() override { return false; }
};

extern static_storage<class devfs> devfs;

class console_inode : public inode_impl<console_inode> {
  os::list<pcb_t *> wait;
  spinlock lock;
public:
  using inode_impl::inode_impl;
  console_inode(): inode_impl(devfs, /*uid=*/0, /*gid=*/0) {
    size = 0; type = CharDevice;
    flags = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  int create(const string &, filetype) override { return -ENOTDIR; }
  inode *lookup(const string &) override { return nullptr; }
  vector<item> list() override { return {}; }

  void wake();
};

class devroot : public inode_impl<devroot> {
  hashmap<string, inode*> children;
public:
  using inode_impl<devroot>::inode_impl;
  size_t read(size_t, void *, size_t, int) override { return 0; }
  size_t write(size_t, const void *, size_t, int) override { return -EROFS; }
  int create(const string &, inode::filetype) override { return -EROFS; }
  inode *lookup(const string &name) override {
    return children[name];
  }
  vector<item> list() override {
    vector<item> result;
    result.reserve(children.size());
    for (auto [name, inode] : children)
      result.push_back({ .handle = (long) inode, .name = name });
    return result;
  }

  // Special registration function.
  void record(const string &name, inode *node) {
    children[name] = node;
  }
};

class block_inode : public inode_impl<block_inode> {
  block_device *dev;
public:
  using inode_impl::inode_impl;
  block_inode(block_device *dev): inode_impl(devfs, /*uid=*/0, /*gid=*/0), dev(dev) {
    size = 0; type = BlockDevice;
    flags = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  int create(const string &, filetype) override { return -ENOTDIR; }
  inode *lookup(const string &) override { return nullptr; }
  vector<item> list() override { return {}; }
};


extern static_storage<console_inode> console;

void mount_dev();

}

#endif
