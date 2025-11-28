#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

namespace os {

// This inode does not own the data, and will not delete it on destruction.
class tmpfs_inode : public os::inode_impl<tmpfs_inode> {
  void *data = nullptr;
  size_t sz = 0;

  os::hashmap<string, inode*> children;
public:
  tmpfs_inode(class fs *fs): inode_impl(fs) { }

  size_t read(size_t offset, void* buf, size_t len, int flags) override;
  size_t write(size_t offset, const void* buf, size_t len, int flags) override;

  result create(const string &name, filetype ty) override;
  inode *lookup(const string &name) override;
  os::vector<inode *> list() override;

  void load(void *data, size_t sz);
  void to_dir() { type = Dir; }
};

class tmpfs : public fs {
public:
  tmpfs();
  tmpfs_inode *get() override { return new tmpfs_inode(this); }
  void erase(inode *) override { }
};

extern static_storage<class tmpfs> tmpfs;
void mount_tmp();

}

#endif
