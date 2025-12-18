#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

#define READONLY_DIRECTORY \
  int create(const string &, filetype, int) override { return -EACCES; } \
  int unlink(const string &) override { return -EACCES; }

namespace os {

class net_device;

// All pseudo-files.
namespace proc {

class filesystems : public inode_impl<filesystems> {
  inode::meta meta;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  filesystems(class fs *fs): inode_impl(fs, 0, 0, 0444, File) {}
  size_t read(size_t, void *, size_t, int) override;
  size_t write(size_t, const void *, size_t, int) override { return -EPERM; }
};

}

class procroot : public inode_impl<procroot> {
  proc::filesystems *filesystems;
  inode::meta meta;
public:
  DIR_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_DIRECTORY;

  procroot(class fs *fs);
  inode *lookup(const string &name) override;
  vector<item> list() override;
};

// An empty FS that does nothing.
class procfs : public fs {
public:
  procfs();
  inode *get() override { return nullptr; }
  void erase(inode *) override {}
  bool has_backup() override { return false; }
};

expected<fs*> procfs_creator(const char *);

}

#endif
