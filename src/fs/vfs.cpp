#include "vfs.h"

namespace os {

static_storage<vfs> vfs_static;

void inode::add_child(inode *child) {
  child->parent = this;
  children[child->name] = child;
}

size_t file::read(void *buf, size_t len) {
  auto ret = node->read(offset, buf, len);
  offset += len;
  return ret;
}

size_t file::write(const void *buf, size_t len) {
  auto ret = node->write(offset, buf, len);
  offset += len;
  return ret;
}

size_t file::seek(long pos, whence whence) {
  size_t before = offset;
  switch (whence) {
  case begin:
    offset = pos;
    break;
  case current:
    offset += pos;
    break;
  }
  return before;
}

int file::close() {
  // Do nothing?
  return 0;
}

inode *vfs::lookup(const string &path) {
  printk("looking up: %s\n", path.c_str());
  // We assume the first mounted FS is the root of the entire VFS.
  if (mounts.empty())
    return nullptr;

  if (path == "/")
    return mounts[0].root;

  inode *current = mounts[0].root;
  if (!current || current->type != inode::Dir)
    return nullptr;

  for (auto name : split(path, "/")) {
    if (name == "" || name == ".")
      continue;

    if (name == "..") {
      if (!(current = current->parent))
        return nullptr;
    }

    if (current->children.count(name)) {
      current = current->children[name];
      // TODO: switch according to mount point.
      continue;
    }

    return nullptr;
  }
  return current;
}

file *vfs::open(const string &path, int flags) {
  inode *node = lookup(path);
  if (!node)
    return nullptr;

  file *f = new file(node, flags);
  node->refcnt++;
  return f;
}

}