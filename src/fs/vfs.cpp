#include "vfs.h"
#include "../proc/schedule.h"

namespace os {

static_storage<os::hashmap<string, expected<fs*>(*)(const char *)>> vfs::creators;
spinlock vfs::mountlock;
// TODO: make it an LRU cache.
static_storage<os::hashmap<pair<inode*, string>, dentry*>> vfs::dcache;
static_storage<os::vector<fs*>> vfs::tosync;
static_storage<os::vector<vfs::mount_data>> vfs::mounted;

inode *file::node() const {
  return entry->node;
}

inode::~inode() {
  if (cache)
    delete cache;
}

ssize_t file::read(void *buf, size_t len) {
  [[likely]] if (!node()->cache) {
    auto ret = node()->read(offset, buf, len, flags);
    if ((ssize_t) ret < 0)
      return ret;

    offset += ret;
    return ret;
  }

  // We have a page cache and have to read from it.
  ssize_t read = 0;
  while (read < long(len) && offset < len) {
    size_t poff = offset % PAGE_SIZE;
    page_cache::page *page = (*node()->cache)[offset / PAGE_SIZE];

    auto chunk = min(len - read, PAGE_SIZE - poff);
    memcpy((char*) buf + read, page->data + poff, chunk);

    read += chunk;
    offset += chunk;
  }
  return read;
}

ssize_t file::write(const void *buf, size_t len) {
  if (flags & O_APPEND)
    offset = node()->size();

  [[likely]] if (!node()->cache) {
    auto ret = node()->write(offset, buf, len, flags);
    if ((ssize_t) ret < 0)
      return ret;

    offset += ret;
    return ret;
  }

  size_t written = 0;
  while (written < len) {
    size_t poff = offset % PAGE_SIZE;
    auto *page = (*node()->cache)[offset / PAGE_SIZE];

    size_t chunk = min(len - written, PAGE_SIZE - poff);
    memcpy(page->data + poff, (char*) buf + written, chunk);

    page->dirty = true;
    written += chunk;
    offset += chunk;
  }
  // We must update file size.
  if (offset >= node()->size())
    node()->cache->flush();
  return written;
}

ssize_t file::seek(long pos, whence whence) {
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

void file::close() {
  node()->onclose(flags);
  drop();
}

// Finds the next path to look up, given the current path and the symlink target.
string resolve_link(string path, const string &link) {
  return link[0] == '/' ? link : normalize(path + "/" + link);
}

bool readable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid != 0) {
    if (!(flags & (1 << (bit + 2))))
      return false;
  }
  return true;
}

bool writable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid != 0) {
    if (!(flags & (1 << (bit + 1))))
      return false;
  }
  return true;
}

bool executable(int uid, int gid, const inode *node) {
  int bit = uid == node->uid ? 6 : gid == node->gid ? 3 : 0;
  int flags = node->mode;
  if (uid == 0) {
    // For root, we need at least one execute bit for files.
    if (node->type != inode::Dir)
      return bool((flags & 1) | (flags & (1 << 3)) | (flags & (1 << 6)));

    // But for directories, we can always do it.
    return true;
  }

  if (!(flags & (1 << (bit + 0))))
    return false;
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
    // Some filesystems can only handle name length <= 255.
    if (name.size() > 255)
      return -ENAMETOOLONG;

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
        return -EACCES;

    auto key = pair { cur->node, name };
    if (auto it = dcache->find(key); it != dcache->end())
      cur = (*it).second;
    else {
      auto inode = cur->node->lookup(name);
      if (!inode)
        return -ENOENT;

      dentry *child = new dentry(name, inode, cur->belong, cur);
      dcache->insert(key, child);
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
        return -EACCES;

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

// When we're instrumenting functions, the lookup_impl function will take up more stack than normal.
#if defined(FUNC_INSTRUMENT) || !defined(NDEBUG)
constexpr static int maxdepth = 8;
#else
constexpr static int maxdepth = 12;
#endif

expected<dentry*> vfs::lookup(const string &path, bool lastsym) {
  // Put a maximum on recursion depth to avoid infinite loops.
  return lookup_impl(path, base, lastsym, maxdepth);
}

expected<dentry*> vfs::lookup_from(const string &path, dentry *dentry, bool lastsym) {
  bool relative = path[0] != '/';
  return lookup_impl(path, relative ? dentry : base, lastsym, maxdepth);
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
  // We don't use negative entries (i.e. explicit noent), because it would be hard to determine when to put it back,
  // when the same file is created again.
  dcache->erase({ node, name });
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
  f->close();
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
  auto fs = (*creators)[fsname](src);
  if ((*fs)->has_backup())
    tosync->push_back(*fs);
  return fs;
}

void vfs::record(const string &fsname, expected<fs*>(*creator)(const char*)) {
  (*creators)[fsname] = creator;
}

vector<string> vfs::recorded_fs() {
  vector<string> result;
  result.reserve(creators->size());
  for (const auto &[name, _] : *creators)
    result.push_back(name);
  return result;
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
  node()->ref();
}

#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT)
void file::ondrop() {
  int cnt = refcnt.load();
  printk("dropped %s, refcnt: %d -> %d\n", entry->path().c_str(), cnt, cnt - 1);
}

void file::onref() {
  int cnt = refcnt.load();
  printk("referred %s, refcnt: %d -> %d\n", entry->path().c_str(), cnt, cnt + 1);
}
#endif

string dentry::path() const {
  string result = name;
  for (const dentry *p = parent; p && p->parent != p; p = p->parent)
    result = p->name + "/" + result;
  
  return "/" + result;
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
  tosync.construct();
  mounted.construct();
}

page_cache::page::page(page_cache *parent, size_t index) : parent(parent), data((char*) as_va(pframe())), index(index), dirty(false) {
  int len = parent->node->read(index * PAGE_SIZE, data, PAGE_SIZE, 0);
  if (len < 0)
    printk("page cache: warning: read failed\n");
  if (len >= 0)
    memset(data + len, 0, PAGE_SIZE - len);
}

page_cache::page::~page() {
  if (dirty)
    parent->node->write(index * PAGE_SIZE, data, PAGE_SIZE, 0);
  pfree((pa_t) data - KERNEL_OFFSET);
}

page_cache::page *page_cache::operator[](size_t i) {
  synchronized _(lock);
  auto it = pages.find(i);
  if (it != pages.end())
    return (*it).second;

  auto page = new class page(this, i);
  pages.insert(i, page);
  return page;
}

void page_cache::erase(size_t i) {
  synchronized _(lock);
  auto it = pages.find(i);
  if (it == pages.end())
    return;

  pages.erase(i);
  delete (*it).second;
}

void page_cache::flush() {
  synchronized _(lock);
  auto oldpage = pages;
  pages.clear();

  // We do a copy to avoid concurrency issues.
  for (auto [_, page] : oldpage)
    delete page;
}

page_cache::~page_cache() {
  flush();
}

}
