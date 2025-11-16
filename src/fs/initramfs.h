#ifndef INITRAMFS_H
#define INITRAMFS_H

#include "../utils/helper.h"
#include "vfs.h"

/*
For CPIO format, refer to https://man.archlinux.org/man/cpio.5.en.
*/
namespace os {

struct cpio_newc_header_t {
  char magic[6];
  char ino[8];
  char mode[8];
  char uid[8];
  char gid[8];
  char nlink[8];
  char mtime[8];
  char filesize[8];
  char devmajor[8];
  char devminor[8];
  char rdevmajor[8];
  char rdevminor[8];
  char namesize[8];
  char check[8];
};

static_assert(sizeof(cpio_newc_header_t) == 110);

void mount_initramfs();

class initramfs_inode : public inode {
  void *data;
public:
  initramfs_inode(inode *parent, filetype type, const string &name, size_t size, void *data):
    inode(parent, type, name, size), data(data) {}

  size_t read(size_t offset, void *buf, size_t len) override;
  size_t write(size_t offset, const void *buf, size_t len) override;
};

}

#endif
