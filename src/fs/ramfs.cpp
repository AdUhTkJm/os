#include "ramfs.h"

namespace os {

void ramfs_inode::load(void *data, size_t sz) {
  this->data = data;
  this->sz = sz;
}

size_t ramfs_inode::read(size_t offset, void *buf, size_t len, int) {
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, (char *) data + offset, l);
  return l;
}

size_t ramfs_inode::write(size_t offset, const void *buf, size_t len, int) {
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy((char *) data + offset, buf, l);
  return l;
}

result ramfs_inode::create(const string &name, filetype ty) {
  if (type != Dir)
    return result::failure;

  auto node = cast<ramfs_inode>(fs->get());
  node->type = ty;
  children[name] = node;
  return result::success;
}

os::vector<inode*> ramfs_inode::list() {
  os::vector<inode*> result;
  for (auto [_, f] : children)
    result.push_back(f);
  return result;
}

inode *ramfs_inode::lookup(const string &name) {
  return children[name];
}

ramfs::ramfs() {
  auto node = get();
  node->to_dir();
  root = new dentry("tmp", node);
}

static_storage<class ramfs> ramfs;

void mount_ramfs() {
  ramfs.construct();
  auto mountpoint = vfs->lookup("/tmp");
  vfs->mount("ram", mountpoint, ramfs->root);
}

}
