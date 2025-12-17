#include "tmpfs.h"
#include "../proc/schedule.h"

namespace os {

void tmpfs_inode::load(void *dat, size_t sz) {
  data.resize(sz);
  memcpy(data.data(), dat, sz);
}

size_t tmpfs_inode::read(size_t offset, void *buf, size_t len, int) {
  ssize_t l = min(long(data.size()) - long(offset), long(len));
  if (l <= 0)
    return 0;
  memcpy(buf, data.data() + offset, l);
  meta.atime = now();
  return l;
}

size_t tmpfs_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  bool append = flags & O_APPEND;
  offset = append ? data.size() : offset;
  if (data.size() < offset + len)
    data.resize(offset + len);
  memcpy(data.data() + offset, buf, len);
  meta.atime = meta.mtime = now();
  return len;
}

int tmpfs_inode::create(const string &name, filetype ty, int mode) {
  if (type != Dir)
    return -ENOTDIR;

  auto node = cast<tmpfs_inode>(fs->get());
  auto tcb = active();
  node->type = ty;
  node->uid = tcb->pcb->uid;
  node->gid = tcb->pcb->gid;
  node->mode = mode;
  children[name] = node;
  meta.atime = meta.mtime = now();
  return 0;
}

int tmpfs_inode::unlink(const string &name) {
  if (!children.count(name))
    return -ENOENT;

  auto node = children[name];
  node->unlinked();

  children.erase(name);
  meta.atime = meta.mtime = now();
  return 0;
}

os::vector<inode::item> tmpfs_inode::list() {
  os::vector<item> result;
  for (auto [name, inode] : children)
    result.push_back({ .inum = (long) inode, .name = name, .ty = inode->type });
  meta.atime = now();
  return result;
}

inode *tmpfs_inode::lookup(const string &name) {
  meta.atime = now();
  if (!children.count(name))
    return nullptr;
  return children[name];
}

tmpfs::tmpfs(int uid, int gid): uid(uid), gid(gid) {
  auto node = get();
  node->type = inode::Dir;
  node->mode = 0777;
  root = new dentry("tmp", node, nullptr);
}

static_storage<class tmpfs> tmpfs;

void mount_tmp() {
  tmpfs.construct(0, 0);
  auto tcb = active();
  auto mountpoint = tcb->pcb->vfs->lookup("/tmp");
  assert(mountpoint);
  vfs::mount(*mountpoint, tmpfs->root);
}

expected<fs*> tmp_creator(const char*) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  return new class tmpfs(pcb->uid, pcb->gid);
}

}
