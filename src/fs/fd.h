#ifndef FD_H
#define FD_H

#include "../utils/helper.h"

namespace os {

class inode {
public:
  virtual ~inode() = default;
  virtual size_t read(size_t offset, void* buf, size_t len) = 0;
  virtual size_t write(size_t offset, const void* buf, size_t len) = 0;

  uint32_t size;
};

class initramfs_inode {
  void *data;
  size_t size;
public:
  initramfs_inode(void *data, size_t size): data(data), size(size) {}

  size_t read(size_t offset, void *buf, size_t len);
  size_t write(size_t offset, const void *buf, size_t len);
};

class file {
public:
  inode *node;
  size_t offset;
  int flags;

  enum whence {
    begin, current
  };

  virtual size_t read(void *buf, size_t len);
  virtual size_t write(const void *buf, size_t len);
  // virtual size_t seek(size_t off, whence whence);
  virtual int close();
};

}

#endif
