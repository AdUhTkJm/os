#include "procfs.h"

namespace os::proc {

size_t filesystems::read(size_t offset, void *buf, size_t len, int) {
  auto content = string("\n").join(vfs::recorded_fs()) + "\n";
  if (offset >= content.size())
    return 0;
  size_t l = min(len, content.size() - offset);
  memcpy(buf, content.c_str() + offset, l);
  meta.atime = now();
  return l;
}

}

namespace os {

procroot::procroot(class fs *fs):
  inode_impl(fs, 0, 0, 0555, Dir), filesystems(new proc::filesystems(fs)) {}

inode *procroot::lookup(const string &name) {
  meta.atime = now();
  if (name == "filesystems")
    return filesystems;
  return nullptr;
}

vector<inode::item> procroot::list() {
  meta.atime = now();
  vector<item> result;
  result.push_back({ filesystems->inum(), "filesystems", File });
  return result;
}

procfs::procfs() {
  auto node = new procroot(this);
  root = new dentry("", node, nullptr);
}

expected<fs*> procfs_creator(const char *) {
  return new procfs;
}

}
