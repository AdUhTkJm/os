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
};

extern static_storage<class devfs> devfs;

class console_inode : public inode_impl<console_inode> {
  os::list<pcb_t *> wait;
  spinlock lock;
public:
  using inode_impl::inode_impl;
  console_inode(const string &name): inode_impl(devfs) {
    this->name = name;
    size = 0; type = File;
  }
  size_t read(size_t offset, void *buf, size_t len) override;
  size_t write(size_t, const void*, size_t) override;
  result create(const string &, filetype) override { return result::failure; }
  inode *lookup(const string &) override { return nullptr; }
  vector<inode*> list() override { return {}; }

  void wake();
};

extern static_storage<console_inode> tty0;

void mount_dev();

}

#endif
