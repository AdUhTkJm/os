#include "tmpfs.h"
#include "../proc/schedule.h"

namespace os {

void tmpfs_inode::load(void *dat, size_t sz) {
  data.resize(sz);
  memcpy(data.data(), dat, sz);
  size = sz;
}

size_t tmpfs_inode::read(size_t offset, void *buf, size_t len, int) {
  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, data.data() + offset, l);
  return l;
}

size_t tmpfs_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  bool append = flags & O_APPEND;
  // On append, the file offset should always be at end.
  if (append) {
    data.resize(size += len);
    memcpy(data.data(), buf, len);
  }

  ssize_t l = min(long(size) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(data.data() + offset, buf, l);
  return l;
}

int tmpfs_inode::create(const string &name, filetype ty) {
  if (type != Dir)
    return -ENOTDIR;

  auto node = cast<tmpfs_inode>(fs->get());
  node->type = ty;
  children[name] = node;
  return 0;
}

os::vector<inode::item> tmpfs_inode::list() {
  os::vector<item> result;
  for (auto [name, inode] : children)
    result.push_back({ .handle = (long) inode, .name = name });
  return result;
}

inode *tmpfs_inode::lookup(const string &name) {
  return children[name];
}

tmpfs::tmpfs(int uid, int gid): uid(uid), gid(gid) {
  auto node = get();
  node->type = inode::Dir;
  root = new dentry("tmp", node);
}

static_storage<class tmpfs> tmpfs;

void mount_tmp() {
  tmpfs.construct(0, 0);
  auto mountpoint = vfs->lookup("/tmp");
  assert(mountpoint);
  vfs->mount(*mountpoint, tmpfs->root);
}

expected<fs*> tmp_creator(const char*) {
  pcb_t *pcb = scheduler.active;
  return new class tmpfs(pcb->uid, pcb->gid);
}

}
