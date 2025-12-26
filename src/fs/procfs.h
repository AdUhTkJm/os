#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

namespace os {

class net_device;
struct pcb_t;

// All pseudo-files.
namespace proc {

class filesystems : public inode_impl<filesystems> {
  inode::meta meta;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_FILE;

  filesystems(class fs *fs): inode_impl(fs, 0, 0, 0444, File) {}
  ssize_t read(size_t, void *, size_t, int) override;
};

class meminfo : public inode_impl<meminfo> {
  inode::meta meta;
  string value;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_FILE;

  meminfo(class fs *fs): inode_impl(fs, 0, 0, 0444, File) {}
  ssize_t read(size_t, void *, size_t, int) override;
};

class link : public inode_impl<link> {
  inode::meta meta;
  string lnk;
public:
  SYMLINK_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;

  link(class fs *fs, const string &lnk): inode_impl(fs, 0, 0, 0444, Link), lnk(lnk) {}
  optional<string> readlink() override { return lnk; }
  size_t size() const override { return lnk.size(); };
};

// Things inside `/proc/[pid]/`.
namespace pid {

// No idea what's this; Just return 0 as Linux typically does.
class oom_score_adj : public inode_impl<oom_score_adj> {
  inode::meta meta;
  pcb_t *pcb;
public:
  FILE_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_FILE;

  oom_score_adj(class fs *fs, pcb_t *pcb): inode_impl(fs, 0, 0, 0444, File), pcb(pcb) {}
  ssize_t read(size_t, void *, size_t, int) override;
};

}

class process : public inode_impl<process> {
  inode::meta meta;
  pcb_t *pcb;
  link *exe;
  inode *oom;
public:
  DIR_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_DIRECTORY;

  process(class fs *fs, pcb_t *pcb);
  ~process();
  inode *lookup(const string &name) override;
  vector<item> list() override;
};

}

class procroot : public inode_impl<procroot> {
  proc::filesystems *filesystems;
  proc::meminfo *meminfo;

  os::hashmap<int, proc::process*> pnodes;
  inode::meta meta;
public:
  DIR_INODE_DEFAULT_IMPL;
  META_DEFAULT_IMPL;
  READONLY_DIRECTORY;

  procroot(class fs *fs);
  ~procroot();
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
