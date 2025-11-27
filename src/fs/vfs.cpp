#include "vfs.h"

namespace os {

static_storage<class vfs> vfs;

size_t file::read(void *buf, size_t len) {
  auto ret = node->read(offset, buf, len, flags);
  offset += ret;
  return ret;
}

size_t file::write(const void *buf, size_t len) {
  auto ret = node->write(offset, buf, len, flags);
  offset += ret;
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

result file::close() {
  vfs->close(this);
  return result::success;
}

dentry* vfs::check_mount(dentry* cur) {
  for (const auto& m : mounts) {
    if (m.host->node == cur->node)
      return m.root;
  }
  return nullptr;
}

dentry *vfs::lookup(const string &path) {
  // We assume the first mounted FS is the root of the entire VFS.
  if (mounts.empty())
    return nullptr;

  if (path == "/")
    return mounts[0].root;

  dentry *cur = mounts[0].root;

  for (auto name : split(path, "/")) {
    if (!cur || cur->node->type != inode::Dir)
      return nullptr;

    if (dentry *root = check_mount(cur))
      cur = root;

    if (name == "" || name == ".")
      continue;

    if (name == "..") {
      if (!(cur = cur->parent))
        return nullptr;
    }

    auto pair = os::pair { cur->node, name };
    if (dcache.count(pair)) {
      cur = dcache[pair];
      continue;
    }

    if (auto inode = cur->node->lookup(name)) {
      cur = new dentry(name, inode, cur);
      dcache[pair] = cur;
      continue;
    }

    return nullptr;
  }
  return cur;
}

file *vfs::open(const string &path, int flags) {
  dentry *dentry = lookup(path);
  if (!dentry)
    return nullptr;

  file *f = new file(dentry->node, flags);
  return f;
}

void vfs::close(file *f) {
  if (!--f->refcnt)
    delete f;
}

void vfs::mount(const string &fsname, dentry *host, dentry *root) {
  (void) fsname;
  mounts.push_back({ host, root });
}

void inode::drop() {
  if (--refcnt)
    return;
  
  fs->erase(this);
  delete this;
}

file::~file() {
  node->drop();
}

file::file(inode *node, int flags): node(node), offset(0), flags(flags), refcnt(1) {
  node->ref();
}

}
