#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"
#include "../proc/pcb.h"
#include "../utils/stl/ring_buffer.h"

namespace os {

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
  console_inode(const string &name): inode_impl(devfs, /*uid=*/0, /*gid=*/0) {
    this->name = name;
    size = 0; type = File;
    flags = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  result create(const string &, filetype) override { return result::failure; }
  inode *lookup(const string &) override { return nullptr; }
  vector<inode*> list() override { return {}; }

  void wake();
};

extern static_storage<console_inode> console;

void mount_dev();

}

#endif
