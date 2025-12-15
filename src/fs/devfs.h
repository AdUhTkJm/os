#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"
#include "../proc/pcb.h"
#include "../utils/stl/ring_buffer.h"
#include "../driver/tty/tty.h"

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
  os::vector<tcb_t *> wait;
  spinlock lock;
public:
  FILE_INODE_DEFAULT_IMPL;

  console_inode(): inode_impl(devfs.get(), /*uid=*/0, /*gid=*/0) {
    type = CharDevice;
    mode = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;

  void wake_read() override;
  void wait_on_read() override;
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
      result.push_back({ .inum = (long) inode, .name = name, .ty = inode->type });
    return result;
  }
  optional<string> readlink() override { return nullopt; }

  size_t size() const override { return 0; }
  long inum() const override { return (long) this; }

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
  FILE_INODE_DEFAULT_IMPL;

  block_inode(block_device *dev): inode_impl(devfs.get(), /*uid=*/0, /*gid=*/0), dev(dev) {
    type = BlockDevice;
    mode = 0666; // rw-rw-rw-
  }
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  // poll() is default, as a regular file.

  // Special functionality.
  void flush();
};

class urandom_inode : public inode_impl<urandom_inode> {
  unsigned char key[32], nonce[12];
  unsigned long counter;
public:
  FILE_INODE_DEFAULT_IMPL;

  urandom_inode();
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;

  void add_entropy(unsigned long entropy);
};

class tty_inode : public inode_impl<tty_inode> {
  string line;
public:
  tty::tty tty;

  FILE_INODE_DEFAULT_IMPL;

  tty_inode(console_inode *console);
  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;
  
  void wake_read() override;
  void wait_on_read() override;
};

class null_inode : public inode_impl<null_inode> {
public:
  FILE_INODE_DEFAULT_IMPL;

  null_inode();
  size_t read(size_t, void *, size_t, int) override { return 0; }
  size_t write(size_t, const void*, size_t len, int) override { return len; }
};

extern static_storage<console_inode> console;
extern static_storage<urandom_inode> urandom;

void mount_dev();

}

#endif
