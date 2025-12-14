#include "vfs.h"
#include "../proc/schedule.h"

namespace os {

static_storage<os::hashmap<string, expected<fs*>(*)(const char *)>> vfs::creators;
spinlock vfs::mountlock;
// TODO: make it an LRU cache.
static_storage<os::hashmap<pair<dentry*, string>, dentry*>> vfs::dcache;

inode *file::node() {
  return entry->node;
}

size_t file::read(void *buf, size_t len) {
  auto ret = node()->read(offset, buf, len, flags);
  if ((ssize_t) ret < 0)
    return ret;

  offset += ret;
  return ret;
}

size_t file::write(const void *buf, size_t len) {
  if (flags & O_APPEND)
    offset = node()->size();
  
  auto ret = node()->write(offset, buf, len, flags);
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
    offset = node()->size() + pos;
    break;
  }
  return before;
}

// Finds the next path to look up, given the current path and the symlink target.
string resolve_link(string path, const string &link) {
  return link[0] == '/' ? link : normalize(path + "/" + link);
}

bool readable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid != 0) {
    if (!(flags & (1 << bit + 2)))
      return false;
  }
  return true;
}

bool writable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid != 0) {
    if (!(flags & (1 << bit + 1)))
      return false;
  }
  return true;
}

bool executable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid != 0) {
    if (!(flags & (1 << bit + 0)))
      return false;
  }
  return true;
}

bool dentry::same(const dentry *other) const {
  if (this == other)
    return true;
  if (!node->same(other->node))
    return false;
  return path() == other->path();
}

expected<dentry*> vfs::lookup_impl(const string &path, dentry *from, bool lastsym, int depth) {
  if (depth < 0)
    return -ELOOP;

  dentry *cur = (path.size() > 0 && path[0] == '/') ? base : from;

  vector<string> comps;
  for (auto comp : split(path, "/"))
    comps.push_back(comp);

  pcb_t *pcb = active()->pcb;
  int uid = pcb->euid, gid = pcb->egid;

  for (size_t i = 0; i < comps.size(); i++) {
    const string &name = comps[i];
    if (!cur)
      return -ENOTDIR;

    {
      synchronized syn(mountlock);
      if (cur->mnt)
        cur = cur->mnt->root;
    }

    if (name == "" || name == ".")
      continue;

    if (name == "..") {
      // Prevent escape.
      if (cur->same(base))
        continue;

      if (cur->same(cur->belong->root)) {
        if (cur->belong->parent)
          cur = cur->belong->host->parent;
      } else if (cur->parent)
        cur = cur->parent;
      continue;
    }

    if (cur->node->type != inode::Dir)
        return -ENOTDIR;

    if (!executable(uid, gid, cur->node))
        return -EPERM;

    auto key = pair { cur, name };
    if (dcache->count(key))
      cur = (*dcache)[key];
    else {
      auto inode = cur->node->lookup(name);
      if (!inode)
        return -ENOENT;

      dentry *child = new dentry(name, inode, cur->belong, cur);
      (*dcache)[key] = child;
      cur = child;

      {
        synchronized syn(mountlock);
        for (auto m : cur->belong->children) {
          if (m->host->same(cur)) {
            cur->mnt = m;
            break;
          }
        }
      }
    }

    // Handle symlink in the middle of the path.
    if (cur->node->type == inode::Link && i + 1 < comps.size()) {
      if (!readable(uid, gid, cur->node))
        return -EPERM;

      auto link = cur->node->readlink();
      if (!link)
        return -ENOENT;

      string resolved = resolve_link(cur->parent->path(), *link);

      // Append the rest of the path.
      for (size_t j = i + 1; j < comps.size(); ++j)
        resolved += "/" + comps[j];

      return lookup_impl(resolved, from, lastsym, depth - 1);
    }
  }

  // Final component symlink resolution.
  if (lastsym && cur->node->type == inode::Link) {
    auto link = cur->node->readlink();
    if (!link)
      return -ENOENT;

    string resolved = resolve_link(cur->parent->path(), *link);
    return lookup_impl(resolved, from, lastsym, depth - 1);
  }

  // Final mount traversal.
  if (lastsym && cur->mnt)
    cur = cur->mnt->root;

  return cur;
}

expected<dentry*> vfs::lookup(const string &path, bool lastsym) {
  // Put a maximum on recursion depth to avoid infinite loops.
  return lookup_impl(path, base, lastsym, 40);
}

expected<dentry*> vfs::lookup_from(const string &path, dentry *dentry, bool lastsym) {
  bool relative = path[0] != '/';
  return lookup_impl(path, relative ? dentry : base, lastsym, 40);
}

// Moves the mount from `source` to `target`.
int vfs::move_mount(dentry *src, dentry *dst) {
  synchronized syn(mountlock);
  mount_t *mount = src->mnt;
  if (!mount)
    return -EINVAL;

  if (dst->mnt)
    return -EBUSY;

  assert(mount->host->same(src));
  if (dst->belong == mount)
    return -ELOOP;

  // Detach from old parent.
  if (mount_t *parent = mount->parent)
    parent->children.erase(mount);
  
  // The mount now becomes a chilren to the mount point of dst.
  mount->host = dst;
  mount->parent = dst->belong;
  mount->root->parent = dst->parent;
  mount->root->name = dst->name;
  
  // The new host dentry now points to the moved mount.
  dst->mnt = mount;
  
  // Add the mount to its new parent's children list.
  if (dst->belong)
    dst->belong->children.push_back(mount);

  src->mnt = nullptr;
  dcache->clear();
  return 0;
}

int vfs::chroot(dentry *entry) {
  if (entry == base)
    return -EINVAL;

  base = entry;
  base->parent = base;
  base->name = "";
  // Base should not be a mount point (we should have followed it).
  assert(!base->mnt);
  
  // After chroot, we must also change pwd.
  auto pcb = active()->pcb;
  pcb->pwd = base;

  dcache->clear();
  return 0;
}

void vfs::invalidate(inode *node, const string &name) {
  (void) node; (void) name;
}

file *vfs::open(const string &path, int flags) {
  auto dentry = lookup(path);
  if (!dentry)
    return nullptr;

  file *f = new file(*dentry, flags);
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
  root->parent = host->parent;
  root->name = host->name;
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
  default:
    return 0;
  }
  __builtin_unreachable();
}

file::~file() {
  node()->drop();
}

file::file(dentry *entry, int flags): entry(entry), offset(0), flags(flags) {
  refcnt = 0;
  node()->ref();
}

string dentry::path() const {
  string result = name;
  for (const dentry *p = parent; p && p->parent != p; p = p->parent)
    result = p->name + "/" + result;
  
  if (result == "")
    return "/";
  return result;
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
