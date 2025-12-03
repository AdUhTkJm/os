#include "vfs.h"

namespace os {

static_storage<class vfs> vfs;

size_t file::read(void *buf, size_t len) {
  auto ret = node->read(offset, buf, len, flags);
  if ((ssize_t) ret < 0)
    return ret;

  offset += ret;
  return ret;
}

size_t file::write(const void *buf, size_t len) {
  if (flags & O_APPEND)
    offset = node->size();
  
  auto ret = node->write(offset, buf, len, flags);
  if ((ssize_t) ret < 0)
    return ret;

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
  case end:
    offset = node->size() + pos;
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

// Finds the next path to look up, given the current path and the symlink target.
string resolve_link(string path, const string &link) {
  return link[0] == '/' ? link : normalize(path + "/" + link);
}

expected<dentry*> vfs::lookup_impl(const string &path, bool lastsym, int depth) {
  if (depth < 0)
    return -ENOENT;

  // We assume the first mounted FS is the root of the entire VFS.
  assert(!mounts.empty());

  if (path == "/")
    return mounts[0].root;

  dentry *cur = mounts[0].root;
  vector<string> comps;

  for (auto name : split(path, "/")) {
    comps.push_back(name);
    if (!cur)
      return -ENOTDIR;

    if (cur->node->type == inode::Link) {
      auto link = cur->node->readlink();
      if (!link)
        return -ENOENT;
      comps.pop_back();
      string v = resolve_link(string("/").join(comps), *link);
      printk("link resolved to %s\n", v.c_str());
      return lookup_impl(v, lastsym, depth - 1);
    }
    
    if (cur->node->type != inode::Dir)
      return -ENOTDIR;

    if (dentry *root = check_mount(cur))
      cur = root;

    if (name == "" || name == ".")
      continue;

    if (name == "..") {
      // Only `..` won't have a parent.
      if (cur->parent)
        cur = cur->parent;
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

    return -ENOENT;
  }

  if (dentry *root = check_mount(cur))
    cur = root;
  
  if (lastsym && cur->node->type == inode::Link) {
    auto link = cur->node->readlink();
    if (!link)
      return -ENOENT;

    string v = resolve_link(dirname(path), *link);
    printk("link resolved to %s\n", v.c_str());
    return lookup_impl(v, lastsym, depth - 1);
  }
  return cur;
}

expected<dentry*> vfs::lookup(const string &path, bool lastsym) {
  // Put a maximum on recursion depth to avoid infinite loops.
  return lookup_impl(normalize(path), lastsym, 40);
}

file *vfs::open(const string &path, int flags) {
  auto dentry = lookup(path);
  if (!dentry)
    return nullptr;

  file *f = new file((*dentry)->node, flags);
  return f;
}

void vfs::close(file *f) {
  f->drop();
}

void vfs::mount(dentry *host, dentry *root) {
  mounts.push_back({ host, root });
}

expected<fs*> vfs::get(const string &fsname, const char *src) {
  if (!creators.count(fsname))
    return -EINVAL;
  return creators[fsname](src);
}

void vfs::record(const string &fsname, expected<fs*>(*creator)(const char*)) {
  creators[fsname] = creator;
}

void inode::drop() {
  if (--refcnt)
    return;
  
  bool backed = fs->has_backup();
  if (lnkcnt == 0)
    fs->erase(this);
  if (backed || lnkcnt == 0)
    delete this;
}

void inode::unlinked() {
  if (--lnkcnt)
    return;

  if (refcnt == 0) {
    fs->erase(this);
    delete this;
  }
}

unsigned char inode::as_dt(filetype ty) {
  switch (ty) {
  case BlockDevice:
    return DT_BLK;
  case File:
    return DT_REG;
  case Dir:
    return DT_DIR;
  case FIFO:
    return DT_FIFO;
  case CharDevice:
    return DT_CHR;
  case Link:
    return DT_LNK;
  case Socket:
    return DT_SOCK;
  }
  __builtin_unreachable();
}

file::~file() {
  node->drop();
}

file::file(inode *node, int flags): refcnt(1), node(node), offset(0), flags(flags) {
  node->ref();
}

void file::drop() {
  if (!--refcnt)
    delete this;
}

string basename(const string &path) {
  if (path.empty())
    return ".";

  // Remove trailing slashes.
  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/')
    --end;

  // Find last '/'.
  size_t pos = path.rfind('/', end - 1);
  if (pos == string::npos)
    return path.substr(0, end);

  return path.substr(pos + 1, end - pos - 1);
}

string dirname(const string &path) {
  if (path.empty())
    return ".";

  size_t end = path.size();
  while (end > 1 && path[end - 1] == '/')
    --end;

  size_t pos = path.rfind('/', end - 1);
  if (pos == string::npos)
    return ".";

  if (pos == 0)
    return "/";

  return path.substr(0, pos);
}

string normalize(const string &path) {
  vector<string> v;

  for (const auto &part : split(path, "/")) {
    if (part == "" || part == ".")
        continue;
    if (part == "..") {
      if (!v.empty())
        v.pop_back();
      continue;
    }
    v.push_back(part);
  }

  return "/" + string("/").join(v);
}

}
