#ifndef INITRAMFS_H
#define INITRAMFS_H

#include "../utils/helper.h"

/*
For CPIO format, refer to https://man.archlinux.org/man/cpio.5.en.
*/

typedef struct {
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
} cpio_newc_header_t;

#ifdef __cplusplus
static_assert(sizeof(cpio_newc_header_t) == 110);
#endif

C void build_initramfs();

#endif
