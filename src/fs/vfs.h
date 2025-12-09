#ifndef VFS_H
#define VFS_H

#include "../utils/helper.h"
#include "../utils/stl/atomic.h"
#include "../utils/stl/optional.h"

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

#define FD_CLOEXEC  0x1

#define F_GETFD 1
#define F_SETFD 2

#define DT_UNKNOWN	0
#define DT_FIFO	1
#define DT_CHR		2
#define DT_DIR		4
#define DT_BLK		6
#define DT_REG		8
#define DT_LNK		10
#define DT_SOCK 12
#define DT_WHT	14

#define MS_RDONLY	1 /* Readonly. */
#define MS_NOSUID	2 /* Ignore suid and sgid bits.  */
#define MS_NODEV  4 /* Disallow access to device special files.  */
#define MS_NOEXEC	8 /* Disallow program execution.  */
#define MS_SYNCHRONOUS 16 /* Writes are synced at once.  */
#define MS_REMOUNT  32 /* Alter flags of a mounted FS.  */
#define MS_MANDLOCK	64 /* Allow mandatory locks on an FS.  */
#define MS_DIRSYNC	128  /* Directory modifications are synchronous.  */
#define MS_NOSYMFOLLOW 256 /* Do not follow symlinks.  */
#define MS_NOATIME	1024 /* Do not update access times.  */
#define MS_NODIRATIME	2048 /* Do not update directory access times.  */
#define MS_BIND	 4096 /* Bind directory at different place.  */
#define MS_MOVE	 8192
#define MS_REC	16384
#define MS_SILENT	32768
#define MS_POSIXACL	(1 << 16) /* VFS does not apply the umask.  */
#define MS_UNBINDABLE	(1 << 17)
#define MS_PRIVATE	(1 << 18)
#define MS_SLAVE	(1 << 19)
#define MS_SHARED	(1 << 20)
#define MS_RELATIME	(1 << 21)
#define MS_KERNMOUNT	(1 << 22)
#define MS_I_VERSION	(1 << 23)
#define MS_STRICTATIME	(1 << 24)
#define MS_LAZYTIME	(1 << 25)
#define MS_ACTIVE	(1 << 30)
#define MS_NOUSER	(1 << 31)

namespace os {

class inode;
class dentry;

class fs {
public:
  fs() {}
  fs(const fs &) = delete;
  fs &operator=(const fs &) = delete;

  dentry *root;

  // Get a free inode in the FS.
  // This will allocate new memory.
  virtual inode *get() = 0;
  // Mark the inode as unused in the FS.
  // This does not deallocate the inode.
  virtual void erase(inode *) = 0;
  virtual bool has_backup() = 0;
};

// This will increase the inode's refcnt, even though dentry doesn't.
class file {
  atomic<unsigned> refcnt;
public:
  dentry *entry;
  size_t offset;
  int flags;
  enum whence {
    begin, current, end
  };

  void drop();
  void ref() { refcnt++; }

  inode *node();

  file(dentry *node, int flags);
  ~file();

  size_t read(void *buf, size_t len);
  size_t write(const void *buf, size_t len);
  size_t seek(long pos, whence whence); // Returns the old offset.
  void close() { drop(); }
};

class inode {
  atomic<unsigned> refcnt;
protected:
  atomic<unsigned> lnkcnt;
public:
  inode(const inode &) = delete;
  inode &operator=(const inode &) = delete;

  const uint64_t rtti;

  enum filetype { File, Dir, Link, BlockDevice, CharDevice, Socket, FIFO };
  static unsigned char as_dt(filetype ty);

  virtual ~inode() = default;
  virtual size_t read(size_t offset, void* buf, size_t len, int flags) = 0;
  virtual size_t write(size_t offset, const void* buf, size_t len, int flags) = 0;

  // Creates a new, empty file.
  // The `mode` is the access mode.
  virtual int create(const string &name, filetype ty, int mode) = 0;
  virtual int unlink(const string &name) = 0;
  // Looks up a child with the given name.
  virtual inode *lookup(const string &name) = 0;
  virtual optional<string> readlink() = 0;

  struct item {
    long inum;
    string name;
    filetype ty;
  };
  // List all children.
  virtual os::vector<item> list() = 0;
  
  virtual size_t size() = 0;
  virtual long inum() = 0;

  // Mark this inode as unused.
  // Possibly deletes itself when refcount drops to zero.
  void drop();
  void ref() { refcnt++; }
  void unlinked();
  void linked() { lnkcnt++; }
  unsigned nlink() { return lnkcnt; }

  filetype type;
  class fs *fs;
  int mode; // Access mode.
  int uid, gid;

  inode(class fs *fs, int uid, int gid, uint64_t rtti): rtti(rtti), fs(fs), uid(uid), gid(gid) { refcnt = 1; }
};

template<class T>
class inode_impl : public inode {
  static uint64_t class_id() {
    static int unique;
    return (uint64_t) &unique;
  }
public:
  inode_impl(class fs *fs, int uid, int gid): inode(fs, uid, gid, (long) class_id()) {}
  static bool classof(inode *p) {
    return p->rtti == class_id();
  }
};

class dentry;
class vfs : public shared {
private:
  // The way to create a new `fs` structure from the opaque `source`.
  // This is limited by the way of system call; we can't use templates.
  static static_storage<os::hashmap<string, expected<fs*>(*)(const char *)>> creators;
  static spinlock mountlock;
  // TODO: make it an LRU cache.
  static static_storage<os::hashmap<pair<inode*, string>, dentry*>> dcache;

  expected<dentry *> lookup_impl(const string &path, dentry *dentry, bool lastsym, int depth);
public:
  struct mount_t : intrusive_list_node<mount_t> {
    // The path in the host filesystem.
    dentry *host;
    // The root of the mounted filesystem.
    dentry *root;
    // The parent mount.
    mount_t *parent;
    // The submounts.
    intrusive_list<mount_t> children;
    int flags;
  } *base;
  dentry *root = nullptr;

  // Don't copy refcnt.
  vfs() {}
  vfs(const vfs &other): base(other.base), root(other.root) {}
  vfs &operator=(const vfs &other) { base = other.base; root = other.root; return *this; }

  // Returns the (optional) entry and an error code.
  // If `lastsym` is set to false, the last component will not be resolved when it is a symlink.
  expected<dentry *> lookup(const string &path, bool lastsym = true);
  expected<dentry *> lookup_from(const string &path, dentry *dentry, bool lastsym = true);
  // When there is a process, use `pcb->open_file` instead. This is for boot.
  file *open(const string &path, int flags);
  void close(file *f);

  // These change global filesystem topology.
  static void mount(dentry *host, dentry *root, int flags = 0);
  static int move_mount(dentry *source, dentry *target);
  int chroot(mount_t *mnt);

  static void invalidate(inode *node, const string &name);

  // Constructs a new in-memory `fs` structure according to the given fs.
  expected<fs*> get(const string &fsname, const char *src);
  static void record(const string &fsname, expected<fs*>(*creator)(const char*));

  // Initialize the global structure.
  static void init();
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

// Directory entry, as a cache.
// This does not own an inode and will not change the node's refcnt.
class dentry {
public:
  dentry *parent;
  string name;
  inode *node;

  vfs::mount_t *belong;        // The mount that this dentry belongs to.
  vfs::mount_t *mnt = nullptr; // The mount point here.
  
  dentry(const string &name, inode *node, vfs::mount_t *belong, dentry *parent = nullptr):
    parent(parent), name(name), node(node), belong(belong) {}

  string path() const;
};

string dirname(const string &path);
string basename(const string &path);
string normalize(const string &path);

bool readable(int uid, int gid, inode *node);
bool writable(int uid, int gid, inode *node);
bool executable(int uid, int gid, inode *node);

}

#endif
