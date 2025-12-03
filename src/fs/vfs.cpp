#include "vfs.h"

namespace os {

static_storage<os::hashmap<string, expected<fs*>(*)(const char *)>> vfs::creators;
spinlock vfs::mountlock;
// TODO: make it an LRU cache.
static_storage<os::hashmap<pair<inode*, string>, dentry*>> vfs::dcache;

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

// Finds the next path to look up, given the current path and the symlink target.
string resolve_link(string path, const string &link) {
  return link[0] == '/' ? link : normalize(path + "/" + link);
}

expected<dentry*> vfs::lookup_impl(const string &path, bool lastsym, int depth) {
  if (depth < 0)
    return -ENOENT;

  // We assume the first mounted FS is the root of the entire VFS.
  assert(base && "base shouldn't be empty on lookup");

  if (path == "/")
    return base->root;

  dentry *cur = base->root;
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

    {
      synchronized syn(mountlock);
      if (cur->mnt)
        cur = cur->mnt->root;
    }

    if (name == "" || name == ".")
      continue;

    if (name == "..") {
      if (cur == cur->belong->root)
        cur = cur->belong->host;
      else if (cur->parent) // Note that `/` won't have a parent.
        cur = cur->parent;
    }

    auto pair = os::pair { cur->node, name };
    if (dcache->count(pair)) {
      cur = (*dcache)[pair];
      continue;
    }

    if (auto inode = cur->node->lookup(name)) {
      cur = new dentry(name, inode, cur->belong, cur);
      auto children = cur->belong->children;

      // Check whether this is a mount point.
      for (auto *child = children.begin(); child != children.end(); child = child->next) {
        if (child->host->node == cur->node) {
          cur->mnt = child;
          break;
        }
      }

      (*dcache)[pair] = cur;
      continue;
    }
  
    return -ENOENT;
  }

  if (lastsym && cur->mnt)
    cur = cur->mnt->root;
  
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
  return lookup_impl(path, lastsym, 40);
}

// Moves the mount from `source` to `target`.
int vfs::move_mount(dentry *src, dentry *dst) {
  synchronized syn(mountlock);

  if (!src->mnt)
    return -EINVAL;

  if (dst->mnt)
    return -EBUSY;

  assert(src->mnt->host == src);
  if (dst->belong == src->mnt)
    return -ELOOP;

  // Detach from old parent.
  if (mount_t *parent = src->mnt->parent)
    parent->children.erase(src->mnt);
  
  // The mount now becomes a chilren to the mount point of dst.
  src->mnt->host = dst;
  src->mnt->parent = dst->belong;
  
  // The new host dentry now points to the moved mount.
  dst->mnt = src->mnt;
  
  // Add the mount to its new parent's children list.
  if (dst->belong)
    dst->belong->children.push_back(src->mnt);

  src->mnt = nullptr;
  dcache->clear();
  return 0;
}

int vfs::chroot(mount_t *mnt) {
  synchronized syn(mountlock);
  if (mnt == base)
    return -EINVAL;

  base = mnt;
  root = mnt->root;
  base->parent = nullptr;
  base->host = base->root;
  dcache->clear();
  return 0;
}

void vfs::invalidate(inode *node, const string &name) {
  dcache->erase({ node, name });
}

file *vfs::open(const string &path, int flags) {
  auto dentry = lookup(path);
  if (!dentry)
    return nullptr;

  file *f = new file((*dentry)->node, flags);
  f->ref();
  return f;
}

void vfs::close(file *f) {
  f->drop();
}

void vfs::mount(dentry *host, dentry *root, int flags) {
  synchronized syn(mountlock);

  auto mount = new mount_t {
    .host = host, .root = root, .parent = host->belong,
    .children = intrusive_list<mount_t>(), .flags = flags
  };
  root->belong = mount;
  root->parent = host;
  host->belong->children.push_back(mount);
  host->mnt = mount;
}

expected<fs*> vfs::get(const string &fsname, const char *src) {
  if (!creators->count(fsname))
    return -EINVAL;
  return (*creators)[fsname](src);
}

void vfs::record(const string &fsname, expected<fs*>(*creator)(const char*)) {
  (*creators)[fsname] = creator;
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

file::file(inode *node, int flags): refcnt(0), node(node), offset(0), flags(flags) {
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

void vfs::init() {
  dcache.construct();
  creators.construct();
}

}
