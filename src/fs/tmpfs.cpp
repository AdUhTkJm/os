#include "tmpfs.h"
#include "../proc/schedule.h"

namespace os {

void tmpfs_inode::load(void *data, size_t sz) {
  this->data = data;
  this->sz = sz;
}

size_t tmpfs_inode::read(size_t offset, void *buf, size_t len, int) {
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, (char *) data + offset, l);
  return l;
}

size_t tmpfs_inode::write(size_t offset, const void *buf, size_t len, int) {
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy((char *) data + offset, buf, l);
  return l;
}

result tmpfs_inode::create(const string &name, filetype ty) {
  if (type != Dir)
    return result::failure;

  auto node = cast<tmpfs_inode>(fs->get());
  node->type = ty;
  children[name] = node;
  return result::success;
}

os::vector<inode*> tmpfs_inode::list() {
  os::vector<inode*> result;
  for (auto [_, f] : children)
    result.push_back(f);
  return result;
}

inode *tmpfs_inode::lookup(const string &name) {
  return children[name];
}

tmpfs::tmpfs(int uid, int gid): uid(uid), gid(gid) {
  auto node = get();
  node->to_dir();
  root = new dentry("tmp", node);
}

static_storage<class tmpfs> tmpfs;

void mount_tmp() {
  tmpfs.construct(0, 0);
  auto mountpoint = vfs->lookup("/tmp");
  assert(mountpoint);
  vfs->mount(*mountpoint, tmpfs->root);
}

fs *tmp_creator(const char*) {
  pcb_t *pcb = scheduler.active;
  return new class tmpfs(pcb->uid, pcb->gid);
}

}
