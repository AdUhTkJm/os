#include "devfs.h"
#include "../driver/plic/plic.h"
#include "../driver/virtio/virtio.h"
#include "../proc/schedule.h"
#include "../utils/errorcode.h"

namespace os {

static_storage<console_inode> console;
static_storage<class devfs> devfs;

size_t console_inode::read(size_t offset, void *buf, size_t len, int flags) {
  bool block = !(flags & O_NONBLOCK);
  (void) offset;
  char *p = (char *) buf;
  for (unsigned i = 0; i < len; i++) {
    optional<char> c = console_input_buf->pop_front();
    while (!c) {
      if (!block)
        return i == 0 ? -EAGAIN : i;
      wait.push_back(scheduler.active);
      suspend();
      c = console_input_buf->pop_front();
    }
    p[i] = *c;
  }
  return len;
}

size_t console_inode::write(size_t offset, const void *buf, size_t len, int) {
  (void) offset;
  char *p = (char *) buf;
  for (unsigned i = 0; i < len; i++)
    kputch(p[i]);
  
  return len;
}

void console_inode::wake() {
  synchronized syn(lock);
  if (!wait.size())
    return;
  auto front = wait.front();
  wait.pop_front();
  scheduler.wakeup(front);
}

size_t block_inode::read(size_t offset, void *buf, size_t len, int) {
  if (len == 0)
    return 0;
  
  char tmp[512];
  size_t total = 0;
  char* dst = (char*) buf;
  const size_t SECTOR_SIZE = 512;

  size_t soff = offset % SECTOR_SIZE;
  size_t start = offset / SECTOR_SIZE;

  // A partial sector read.
  if (soff > 0) {
    if (dev->read(start, tmp) != 0)
      return 0;

    size_t sz = min(len, SECTOR_SIZE - soff);
    memcpy(dst, tmp + soff, sz);
    
    dst += sz;
    len -= sz;
    total += sz;
    start++;
  }

  // Read full sectors.
  for (size_t i = 0; i < len / SECTOR_SIZE; ++i) {
    if (dev->read(start + i, dst) < 0)
      return total;
    dst += SECTOR_SIZE;
    len -= SECTOR_SIZE;
    total += SECTOR_SIZE;
    start++;
  }

  // A partial sector write.
  if (len > 0) {
    if (dev->read(start, tmp) != 0)
      return total;
    
    memcpy(dst, tmp, len);
    total += len;
  }

  return total;
}

size_t block_inode::write(size_t offset, const void *buf, size_t len, int) {
  if (len == 0)
    return 0;
  
  char tmp[512];
  size_t total = 0;
  const char *src = (const char*) buf;
  const size_t SECTOR_SIZE = 512;

  size_t soff = offset % SECTOR_SIZE;
  size_t start = offset / SECTOR_SIZE;

  if (soff > 0) {
    // We must read first to preserve data in the same sector.
    if (dev->read(start, tmp) != 0)
      return 0;

    size_t sz = min(len, SECTOR_SIZE - soff);
    memcpy(tmp + soff, src, sz);
    
    if (dev->write(start, tmp) != 0)
      return 0;

    src += sz;
    len -= sz;
    total += sz;
    start++;
  }

  // Write full sectors.
  for (size_t i = 0; i < len / SECTOR_SIZE; ++i) {
    if (dev->write(start + i, src) != 0)
      return total;

    src += SECTOR_SIZE;
    len -= SECTOR_SIZE;
    total += SECTOR_SIZE;
    start++;
  }

  // Write the last bits.
  if (len > 0) {
    if (dev->read(start, tmp) != 0)
      return total;
    
    memcpy(tmp, src, len);
    if (dev->write(start, tmp) != 0)
      return total;

    total += len;
  }

  return total;
}

devfs::devfs() {
  auto node = new devroot(this, 0, 0);
  root = new dentry("/dev", node);
}

void mount_dev() {
  auto root = devfs->root;
  // console is initialized in PLIC handler.
  cast<devroot>(root->node)->record("console", &*console);
  auto dentry = vfs->lookup("/dev");
  if (!dentry)
    panic("devfs: cannot find /dev");
  
  vfs->mount(*dentry, root);
}

}