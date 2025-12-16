#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

namespace os {

class procroot : public inode_impl<procroot> {
public:
  DIR_INODE_DEFAULT_IMPL;

  int create(const string &name, filetype ty, int mode) override;
  int unlink(const string &name) override;
  inode *lookup(const string &name) override;
};

// An empty FS that does nothing.
class procfs : public fs {
  inode *get() override { return nullptr; }
  void erase(inode *) override {}
  bool has_backup() override { return false; }
};

expected<fs*> procfs_creator(const char *);

}

#endif
