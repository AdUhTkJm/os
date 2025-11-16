#include "fd.h"

size_t os::initramfs_inode::read(size_t offset, void *buf, size_t len) {
  len = os::min(size - offset, len);
  memcpy(buf, (char *) data + offset, len);
  return len;
}

// This is read-only.
size_t os::initramfs_inode::write(size_t, const void *, size_t) {
  return -1ul;
}

size_t os::file::read(void *buf, size_t len) {
  return node->read(offset, buf, len);
}

size_t os::file::write(const void *buf, size_t len) {
  return node->write(offset, buf, len);
}

int os::file::close() {
  // Do nothing?
  return 0;
}
