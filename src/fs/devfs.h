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
    type = CharDevice;
    mode = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  int create(const string &, filetype, int) override { return -ENOTDIR; }
  int unlink(const string &) override { return -ENOTDIR; }
  inode *lookup(const string &) override { return nullptr; }
  vector<item> list() override { return {}; }

  size_t size() override { return 0; }
  long inum() override { return (long) this; }

  void wake();
};

class devroot : public inode_impl<devroot> {
  hashmap<string, inode*> children;
public:
  devroot(class fs *fs);
  size_t read(size_t, void *, size_t, int) override { return 0; }
  size_t write(size_t, const void *, size_t, int) override { return -EROFS; }
  int create(const string &, inode::filetype, int) override { return -EROFS; }
  int unlink(const string &) override { return -EROFS; }
  inode *lookup(const string &name) override {
    return children[name];
  }
  vector<item> list() override {
    vector<item> result;
    result.reserve(children.size());
    for (auto [name, inode] : children)
      result.push_back({ .inum = (long) inode, .name = name });
    return result;
  }

  size_t size() override { return 0; }
  long inum() override { return (long) this; }

  // Special registration function.
  void record(const string &name, inode *node) {
    children[name] = node;
    node->linked();
  }
};

class block_inode : public inode_impl<block_inode> {
  block_device *dev;

  struct cached_sector {
    unsigned char data[512];
    bool dirty = false;
    bool valid = false;
  };
  // TODO: change into LRU
  os::hashmap<unsigned, cached_sector> cache;

  cached_sector &load_sector(unsigned sector, bool force_reload = false);
  void flush_sector(unsigned sector);
public:
  using inode_impl::inode_impl;
  block_inode(block_device *dev): inode_impl(devfs, /*uid=*/0, /*gid=*/0), dev(dev) {
    type = BlockDevice;
    mode = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  int create(const string &, filetype, int) override { return -ENOTDIR; }
  int unlink(const string &) override { return -ENOTDIR; }
  inode *lookup(const string &) override { return nullptr; }
  vector<item> list() override { return {}; }

  size_t size() override { return 0; }
  long inum() override { return (long) this; }

  // Special functionality
  void flush();
};


extern static_storage<console_inode> console;

void mount_dev();

}

#endif
