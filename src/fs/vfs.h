#ifndef VFS_H
#define VFS_H

#include "../utils/helper.h"

#define O_RDONLY    00000000 /* Open for reading only */
#define O_WRONLY    00000001 /* Open for writing only */
#define O_RDWR      00000002 /* Open for reading and writing */

#define O_CREAT     00000100 /* Create file if it does not exist */
#define O_EXCL      00000200 /* Exclusive use flag (used with O_CREAT) */
#define O_NOCTTY    00000400 /* If path is a terminal, do not make it the controlling terminal */
#define O_TRUNC     00001000 /* Truncate size to 0 */

#define O_APPEND    00002000 /* Writes append to the end of file */
#define O_NONBLOCK  00004000 /* Non-blocking mode */
#define O_SYNC      00010000 /* Write operations finish only when data and metadata are committed to disk */
#define O_FSYNC     O_SYNC   /* Synonym for O_SYNC */

#define O_ASYNC     00020000 /* Enable signal generation (SIGIO) when input or output becomes possible */
#define O_DIRECT    00040000 /* Bypass buffer cache (Direct I/O) */
#define O_DIRECTORY 00200000 /* Fail if the path is not a directory */
#define O_NOFOLLOW  00400000 /* Do not follow symbolic links */
#define O_CLOEXEC   02000000 /* Close file descriptor upon execve() */

namespace os {

class inode;
class dentry;

class fs {
public:
  dentry *root;

  // Get a free inode in the FS.
  // This will allocate new memory.
  virtual inode *get();
  // Mark the inode as unused in the FS.
  // Note that this will not recycle the inode.
  virtual void erase(inode *);
};

class file {
public:
  inode *node;
  size_t offset;
  int flags;
  unsigned refcnt = 0;
  enum whence {
    begin, current
  };

  file() {}
  file(inode *node, int flags): node(node), offset(0), flags(flags) { }

  size_t read(void *buf, size_t len);
  size_t write(const void *buf, size_t len);
  size_t seek(long pos, whence whence);
  result close();
};

class inode {
public:
  const uint64_t rtti;

  enum filetype { File, Dir, Link };

  virtual ~inode() = default;
  virtual size_t read(size_t offset, void* buf, size_t len) = 0;
  virtual size_t write(size_t offset, const void* buf, size_t len) = 0;

  virtual result onclose() { return result::success; }

  // Creates a new, empty file.
  virtual result create(const string &name, filetype ty) = 0;
  // Looks up a child with the given name.
  virtual inode *lookup(const string &name) = 0;
  // List all children.
  virtual os::vector<inode *> list() = 0;

  string name;
  size_t size;
  filetype type;
  unsigned refcnt = 0;
  class fs *fs;

  inode(class fs *fs, uint64_t rtti): rtti(rtti), fs(fs) {}
};

template<class T>
class inode_impl : public inode {
  constexpr static const char *__name() { return __PRETTY_FUNCTION__; }
  constexpr static uint64_t __hash(const char *const &key) {
    uint64_t hash = os::detail::FNV_OFFSET_BASIS;
    for (const char *p = key; *p; p++) {
      hash *= os::detail::FNV_PRIME;
      hash ^= *p;
    }
    return hash;
  };
  constexpr static uint64_t ID = __hash(__name());
public:
  inode_impl(class fs *fs): inode(fs, ID) {}
  static bool classof(inode *p) {
    return p->rtti == ID;
  }
};

// Directory entry, as a cache.
class dentry {
public:
  dentry *parent;
  string name;
  inode *node;
  class fs *fs;
  unsigned refcnt = 0;

  dentry(const string &name, inode *node, dentry *parent = nullptr):
    parent(parent), name(name), node(node), fs(node->fs) {}
};

class vfs {
  struct mount {
    // The path in the host filesystem.
    dentry *host;
    // The root of the mounted filesystem.
    dentry *root;
  };

  os::vector<struct mount> mounts;
  dentry *root = nullptr;
  // TODO: make it an LRU cache.
  os::hashmap<pair<inode*, string>, dentry*> dcache;

  dentry *check_mount(dentry *cur);
public:
  vfs(dentry *root): root(root) { }

  dentry *lookup(const string &path);
  file *open(const string &path, int flags);

  void mount(const string &fsname, dentry *host, dentry *root);
};

class SeekGuard {
  size_t pos;
  file *f;
public:
  SeekGuard(file *f, int offset, file::whence whence = file::begin): f(f) {
    pos = f->seek(offset, whence);
  }
  ~SeekGuard() { f->seek(pos, file::begin); }
};

extern os::static_storage<vfs> vfs;

}

#endif
