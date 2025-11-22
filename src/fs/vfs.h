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
class file {
public:
  inode *node;
  size_t offset;
  int flags;
  int refcnt = 0;
  enum whence {
    begin, current
  };

  file() {}
  file(inode *node, int flags): node(node), offset(0), flags(flags) {}

  virtual size_t read(void *buf, size_t len);
  virtual size_t write(const void *buf, size_t len);
  virtual size_t seek(long pos, whence whence);
  virtual int close();
};


class inode {
public:
  virtual ~inode() = default;
  virtual size_t read(size_t offset, void* buf, size_t len) = 0;
  virtual size_t write(size_t offset, const void* buf, size_t len) = 0;

  // Return 0 for success, and negative values for error.
  virtual int onclose() { return 0; }
  virtual int onseek(long, file::whence) { return 0; }

  void add_child(inode *child);

  os::hashmap<string, inode *> children;
  string name;
  inode *parent;
  size_t size;
  enum filetype { File, Dir } type;
  int refcnt = 0;

  inode() {}
  inode(inode *parent, filetype type, const string &name, size_t size):
    name(name), parent(parent), size(size), type(type) {}
};

struct mount {
  inode *root;
  const char *fs_type;
};

struct vfs {
  vector<mount> mounts;
  inode *lookup(const string &path);
  file *open(const string &path, int flags);
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

extern os::static_storage<vfs> vfs_static;

}

#endif
