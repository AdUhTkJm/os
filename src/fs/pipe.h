#ifndef PIPE_H
#define PIPE_H

#include "vfs.h"
#include "../proc/schedule.h"

namespace os {

class pipe_inode : public inode_impl<pipe_inode> {
  size_t rpos = 0, wpos = 0;
  int readers = 1, writers = 1;

  wait_queue read_wait, write_wait;
  spinlock lock;

  // Try to keep the entire pipe in a page.
  static constexpr size_t capacity = 3922;
  char buffer[capacity];

  size_t available() const { return wpos - rpos; }
  size_t space() const { return capacity - available(); }
  size_t index(size_t pos) const { return pos % capacity; }
public:
  FILE_INODE_DEFAULT_IMPL;

  pipe_inode(os::fs *fs, int uid, int gid);
  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t offset, const void *buf, size_t len, int flags) override;
  short poll(unsigned short) override;
  // Pipes do not support things like `fstatat`.
  meta get_meta() override { return meta(0, 0, 0); }
  void set_meta(const inode::meta &) override {}
  void onclose(int flags) override;

  void prepare_read_wait(wait_entry &) override;
  void prepare_write_wait(wait_entry &) override;

  void finish_read_wait(wait_entry &) override;
  void finish_write_wait(wait_entry &) override;

  void incread() { readers++; }
  void incwrite() { writers++; }
  void incf(const file *f);
};

static_assert(sizeof(pipe_inode) <= PAGE_SIZE);

class pipefs : public fs {
public:
  pipe_inode *get() override {
    auto pcb = active()->pcb;
    return new pipe_inode(this, pcb->euid, pcb->egid);
  }

  void erase(inode *) override { }
  bool has_backup() override { return false; }
  int sync() override { return -EINVAL; }

  // Configuration.

  // Maximum buffer length.
  static const unsigned long maxbuf = 1_mb;
};

extern static_storage<class pipefs> pipefs;

}

#endif
