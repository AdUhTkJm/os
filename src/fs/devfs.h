#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"
#include "../proc/pcb.h"
#include "../utils/stl/ring_buffer.h"
#include "../utils/stl/rbtree.h"
#include "../driver/tty/tty.h"

namespace os {

struct /*interface*/ block_device {
  virtual int read(size_t lba, void *buf, int len) = 0;
  virtual int write(size_t lba, const void *buf, int len) = 0;
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
  wait_queue wait;
  spinlock lock;
  inode::meta meta;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  console_inode(): inode_impl(devfs.get(), 0, 0, 0666, CharDevice) {}
  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;

  void wake_read() override;
  void prepare_read_wait(wait_entry &) override;
  void finish_read_wait(wait_entry &) override;
};

class devroot : public inode_impl<devroot> {
  hashmap<string, inode*> children;
  inode::meta meta;
public:
  DIR_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_DIRECTORY;

  // Is it really read-only?
  devroot(class fs *fs);
  inode *lookup(const string &name) override;
  vector<item> list() override;

  // Special registration function.
  void record(const string &name, inode *node) {
    children[name] = node;
    node->linked();
  }
};

class block_inode : public inode_impl<block_inode> {
  block_device *dev;
  inode::meta meta;

  struct cached_sector : rb_node<unsigned, cached_sector> {
    // We allocate a physical page for it.
    // If we just write `data[4096]`, then there will be ~4KB of padding introduced by VM allocator.
    unsigned char *data = 0;
    bool dirty = false;
    bool busy = false;
  };
  // TODO: change into LRU
  os::rb_tree<unsigned, cached_sector> cache;

  cached_sector *load_page(unsigned page, bool force_reload = false);
  void flush_page(unsigned page);
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  block_inode(block_device *dev): inode_impl(devfs.get(), 0, 0, 0666, BlockDevice), dev(dev) {}
  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t, const void*, size_t, int flags) override;
  // poll() is default, as a regular file.

  // Special functionality.
  void flush();
  void *get_page(unsigned i);
  void mark_dirty(unsigned i);
};

class urandom_inode : public inode_impl<urandom_inode> {
  unsigned char key[32], nonce[12];
  unsigned long counter;
  inode::meta meta;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  urandom_inode();
  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t, const void*, size_t, int flags) override;

  void add_entropy(unsigned long entropy);
};

class tty_inode : public inode_impl<tty_inode> {
  string line;
  inode::meta meta;
public:
  tty::tty tty;

  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  tty_inode(console_inode *console);
  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;
  
  void wake_read() override;
  void prepare_read_wait(wait_entry &) override;
  void finish_read_wait(wait_entry &) override;
};

class null_inode : public inode_impl<null_inode> {
  inode::meta meta;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  null_inode();
  ssize_t read(size_t, void *, size_t, int) override { return 0; }
  ssize_t write(size_t, const void*, size_t len, int) override { return len; }
  int truncate(size_t) override { return 0; }
};

extern static_storage<console_inode> console;
extern static_storage<urandom_inode> urandom;

void mount_dev();

}

#endif
