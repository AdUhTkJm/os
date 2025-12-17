#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

namespace os {

// This inode owns the data.
class tmpfs_inode : public os::inode_impl<tmpfs_inode> {
  vector<char> data;

  os::hashmap<string, tmpfs_inode*> children;
  atomic<unsigned> lnkcnt;
  inode::meta meta;
public:
  META_DEFAULT_IMPL;
  
  tmpfs_inode(class fs *fs, int uid, int gid, int mode, filetype ty): inode_impl(fs, uid, gid, mode, ty) { }

  size_t read(size_t offset, void* buf, size_t len, int flags) override;
  size_t write(size_t offset, const void* buf, size_t len, int flags) override;

  int create(const string &name, filetype ty, int mode) override;
  int unlink(const string &name) override;
  inode *lookup(const string &name) override;
  os::vector<item> list() override;
  optional<string> readlink() override { return nullopt; }\

  size_t size() const override { return data.size(); }
  // Note that `data.data()` will change, so we can't use it.
  long inum() const override { return (long) this; }

  void load(void *data, size_t sz);
};

class tmpfs : public fs {
  int uid, gid;
public:
  tmpfs(int uid, int gid);
  tmpfs_inode *get() override { return new tmpfs_inode(this, uid, gid, 0, inode::Bad); }
  void erase(inode *) override { }
  bool has_backup() override { return false; }
};

extern static_storage<class tmpfs> tmpfs;
void mount_tmp();
expected<fs*> tmp_creator(const char*);

}

#endif
