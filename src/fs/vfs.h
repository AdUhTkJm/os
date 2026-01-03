#ifndef VFS_H
#define VFS_H

#include "../utils/helper.h"
#include "../utils/stl/atomic.h"
#include "../utils/stl/optional.h"
#include "../utils/stl/rbtree.h"
#include "../mem/ptable.h"
#include "../lock/mutex.h"

#define O_RDONLY    0x00000000 /* Open for reading only */
#define O_WRONLY    0x00000001 /* Open for writing only */
#define O_RDWR      0x00000002 /* Open for reading and writing */

#define O_CREAT     0x00000040 /* Create file if it does not exist */
#define O_EXCL      0x00000080 /* Exclusive use flag (used with O_CREAT) */
#define O_NOCTTY    0x00000100 /* If path is a terminal, do not make it the controlling terminal */
#define O_TRUNC     0x00000200 /* Truncate size to 0 */

#define O_APPEND    0x00000400 /* Writes append to the end of file */
#define O_NONBLOCK  0x00000800 /* Non-blocking mode */
#define O_SYNC      0x00001000 /* Write operations finish only when data and metadata are committed to disk */
#define O_FSYNC     O_SYNC     /* Synonym for O_SYNC */

#define O_ASYNC     0x00002000 /* Enable signal generation (SIGIO) when input or output becomes possible */
#define O_DIRECT    0x00004000 /* Bypass buffer cache (Direct I/O) */
#define O_LARGEFILE 0x00008000 /* Allow >= 2GB files. Ignored on 64-bit */
#define O_DIRECTORY 0x00010000 /* Fail if the path is not a directory */
#define O_NOFOLLOW  0x00020000 /* Do not follow symbolic links */
#define O_CLOEXEC   0x00080000 /* Close file descriptor upon execve() */
#define O_PATH      0x00200000 /* Create as a path */

#define FD_CLOEXEC  0x1

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_DUPFD_CLOEXEC 1030 /* Duplicate file descriptor with close-on-exit set.  */

#define DT_UNKNOWN	0
#define DT_FIFO	1
#define DT_CHR	2
#define DT_DIR	4
#define DT_BLK	6
#define DT_REG	8
#define DT_LNK	10
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

#define POLLIN		0x001		/* There is data to read.  */
#define POLLPRI		0x002		/* There is urgent data to read.  */
#define POLLOUT		0x004		/* Writing now will not block.  */
#define POLLERR		0x008		/* Error condition.  */
#define POLLHUP		0x010		/* Hung up.  */
#define POLLNVAL	0x020		/* Invalid polling request.  */

namespace os {

class inode;
class dentry;

// Returns nanoseconds since 1970.1.1.
size_t now();

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

  // Write all cache to disk.
  virtual void sync() {}
};

// This will increase the inode's refcnt, even though dentry doesn't.
class file : public shared {
public:
  dentry *entry;
  // For ordinary file, this is the byte offset;
  // For directory, this is the entry offset;
  // For other things, this is meaningless.
  size_t offset;
  int flags;
  enum whence {
    begin, current, end
  };

  inode *node() const;

  file(dentry *node, int flags);
  ~file();

  ssize_t read(void *buf, size_t len);
  ssize_t write(const void *buf, size_t len);
  ssize_t seek(long pos, whence whence); // Returns the old offset.
  void close();
  void sync();

#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT)
  void ondrop() override;
  void onref() override;
#endif
};

class page_cache {
public:
  mutex lock;

  class page {
  public:
    page_cache *parent;
    char *data;
    size_t index;
    bool dirty = false;

    ~page();
    page(const page &other) = delete;
    page &operator=(const page &other) = delete;
  private:
    friend class page_cache;
    page(page_cache *parent, size_t index);
  };

  page_cache(inode *node): node(node) {}

  page *operator[](size_t i);
  ~page_cache();

  void flush();
  void drop(size_t i);
  // Force-erase, by truncate() or munmap.
  void erase(size_t i);

private:
  os::hashmap<size_t, page*> pages;
  inode *node;
  friend class page_cache::page;
};

class inode {
  atomic<unsigned> refcnt;
protected:
  atomic<unsigned> lnkcnt;
public:
  inode(const inode &) = delete;
  inode &operator=(const inode &) = delete;

  const uint64_t rtti;

  enum filetype { File, Dir, Link, BlockDevice, CharDevice, Socket, FIFO, Bad };
  // Note: this gives nanoseconds!
  struct meta {
    size_t atime; // Access time.
    size_t ctime; // Creation time.
    size_t mtime; // Modification time.
    meta() { atime = ctime = mtime = now(); }
    meta(size_t atime, size_t ctime, size_t mtime): atime(atime), ctime(ctime), mtime(mtime) {}
  };
  
  static unsigned char as_dt(filetype ty);

  virtual ~inode();
  virtual ssize_t read(size_t offset, void* buf, size_t len, int flags) = 0;
  virtual ssize_t write(size_t offset, const void* buf, size_t len, int flags) = 0;
  virtual int truncate(size_t len) { (void) len; return -EINVAL; }

  // Creates a new, empty file.
  // The `mode` is the access mode.
  virtual int create(const string &name, filetype ty, int mode) = 0;
  virtual int unlink(const string &name) = 0;
  virtual int rmdir(const string &name) = 0;
  virtual int move(const string &name, inode *other, const string &newname, int flags) = 0;
  // Looks up a child with the given name.
  virtual inode *lookup(const string &name) = 0;
  virtual optional<string> readlink() = 0;

  // Returns access/creation/modification time.
  virtual meta get_meta() = 0;
  virtual void set_meta(const meta &meta) = 0;

  virtual short poll(unsigned short event) { (void) event; return POLLIN | POLLOUT; }

  virtual void prepare_read_wait(wait_entry &) {}
  virtual void prepare_write_wait(wait_entry &) {}

  virtual void finish_read_wait(wait_entry &) {}
  virtual void finish_write_wait(wait_entry &) {}
  // Note that these functions might not return, as they would potentially preempt.
  virtual void wake_read() {}
  virtual void wake_write() {}

  virtual int onchmod() { return 0; }
  virtual int onchown() { return 0; }
  virtual void onclose(int openflags) { (void) openflags; }

  struct item {
    long inum;
    string name;
    filetype ty;
  };
  // List all children.
  virtual os::vector<item> list() = 0;
  
  virtual size_t size() const = 0;
  virtual long inum() const = 0;

  // Mark this inode as unused.
  // Possibly deletes itself when refcount drops to zero.
  void drop();
  void ref() { refcnt++; }
  void unlinked();
  void linked() { lnkcnt++; }
  unsigned nlink() { return lnkcnt; }
#ifndef NDEBUG
  unsigned inspect_refcnt() { return refcnt; }
#endif

  bool same(const inode *other) const { return inum() == other->inum(); }

  filetype type;
  class fs *fs;
  int mode; // Access mode.
  int uid, gid;
  page_cache *cache = nullptr;

  inode(class fs *fs, int uid, int gid, int mode, filetype type, uint64_t rtti):
    rtti(rtti), type(type), fs(fs), mode(mode), uid(uid), gid(gid) { refcnt = 1; /* lnkcnt implicitly zeroed. */ }
};

template<class T>
class inode_impl : public inode {
  static uint64_t class_id() {
    static int unique;
    return (uint64_t) &unique;
  }
public:
  inode_impl(class fs *fs, int uid, int gid, int mode, filetype type): inode(fs, uid, gid, mode, type, (long) class_id()) {}
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
  // All mounted filesystems that must respond to sync().
  static static_storage<os::vector<fs*>> tosync;

  expected<dentry *> lookup_impl(const string &path, dentry *dentry, bool lastsym, int depth);
public:
  struct mount_data {
    string fstype;
    string device;
    string mntpoint;
    int prot;
  };
  // Since the `device` is 
  static static_storage<os::vector<mount_data>> mounted;

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
  };
  dentry *base;

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
  int chroot(dentry *entry);

  static void invalidate(inode *node, const string &name);

  // Constructs a new in-memory `fs` structure according to the given fs.
  expected<fs*> get(const string &fsname, const char *src);
  static void record(const string &fsname, expected<fs*>(*creator)(const char*));
  // Returns all registered filesystems (ones that we're able to mount).
  static vector<string> recorded_fs();

  // Initialize the global structure.
  static void init();

#ifndef NDEBUG
  static auto &inspect_dcache() { return dcache; }
#endif

  static const vector<fs*> &to_sync() { return *tosync; }
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
  bool same(const dentry *other) const;
};

string dirname(const string &path);
string basename(const string &path);
string normalize(const string &path);

// These check inode permissions.
bool readable(int uid, int gid, const inode *node);
bool writable(int uid, int gid, const inode *node);
bool executable(int uid, int gid, const inode *node);

// These check file permissions.
inline bool can_write(int flags) { return (flags & 0x3) == O_RDWR || (flags & 0x3) == O_WRONLY; }
inline bool can_read(int flags)  { return (flags & 0x3) == O_RDWR || (flags & 0x3) == O_RDONLY; }

#define FILE_INODE_DEFAULT_IMPL \
  int create(const string &, filetype, int) override { return -ENOTDIR; } \
  int unlink(const string &) override { return -ENOTDIR; } \
  int rmdir(const string &) override { return -ENOTDIR; } \
  int move(const string &, inode *, const string &, int) override { return -ENOTDIR; } \
  inode *lookup(const string &) override { return nullptr; } \
  vector<item> list() override { return {}; } \
  optional<string> readlink() override { return nullopt; } \
  size_t size() const override { return 0; } \
  long inum() const override { return (long) this; } \

#define SYMLINK_INODE_DEFAULT_IMPL \
  ssize_t write(size_t, const void*, size_t, int) override { return -EACCES; } \
  ssize_t read(size_t offset, void *buf, size_t len, int) override { auto s = *readlink(); auto l = min(long(s.size()) - long(offset), long(len)); memcpy(buf, s.c_str() + offset, l); return l; } \
  int create(const string &, filetype, int) override { return -ENOTDIR; } \
  int unlink(const string &) override { return -ENOTDIR; } \
  int rmdir(const string &) override { return -ENOTDIR; } \
  int move(const string &, inode *, const string &, int) override { return -ENOTDIR; } \
  inode *lookup(const string &) override { return nullptr; } \
  vector<item> list() override { return {}; } \
  long inum() const override { return (long) this; } \

#define DIR_INODE_DEFAULT_IMPL \
  int truncate(size_t) override { return -EISDIR; } \
  ssize_t read(size_t, void *, size_t, int) override { return -EISDIR; } \
  ssize_t write(size_t, const void *, size_t, int) override { return -EISDIR; } \
  optional<string> readlink() override { return nullopt; } \
  size_t size() const override { return 0; } \
  long inum() const override { return (long) this; } \

#define META_DEFAULT_IMPL \
  inode::meta get_meta() override { return meta; } \
  void set_meta(const inode::meta &meta) override { this->meta = meta; } \

#define READONLY_DIRECTORY \
  int create(const string &, filetype, int) override { return -EACCES; } \
  int unlink(const string &) override { return -EACCES; } \
  int rmdir(const string &) override { return -EACCES; } \
  int move(const string &, inode *, const string &, int) override { return -EACCES; } \

#define READONLY_FILE \
  ssize_t write(size_t, const void *, size_t, int) override { return -EACCES; } \
  int truncate(size_t) override { return -EACCES; }

}

#endif
