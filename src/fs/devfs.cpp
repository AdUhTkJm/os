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

block_inode::cached_sector &block_inode::load_sector(unsigned sector, bool force_reload) {
  auto &c = cache[sector];
  if (c.valid && !force_reload)
    return c;
  
  if (dev->read(sector, c.data) != 0)
    memset(c.data, 0, 512); // Perhaps better notify the failure?
  c.valid = true;
  return c;
}

void block_inode::flush_sector(unsigned sector) {
  auto &c = cache[sector];
  if (!c.valid || !c.dirty)
    return;
  dev->write(sector, c.data);
  c.dirty = false;
}

size_t block_inode::read(size_t offset, void *buf, size_t len, int flags) {
  bool direct = flags & O_DIRECT;
  if (len == 0)
    return 0;

  size_t total = 0;
  size_t sector = offset / 512;
  size_t soff = offset % 512;
  char *dst = (char*) buf;

  // Partial first sector.
  if (soff > 0) {
    auto &c = load_sector(sector, direct);
    size_t sz = min(len, 512 - soff);
    memcpy(dst, c.data + soff, sz);

    dst += sz;
    len -= sz;
    total += sz;
    sector++;
  }

  // Full sectors.
  while (len >= 512) {
    auto &c = load_sector(sector, direct);
    memcpy(dst, c.data, 512);

    dst += 512;
    len -= 512;
    total += 512;
    sector++;
  }

  // Last partial sector.
  if (len > 0) {
    auto &c = load_sector(sector, direct);
    memcpy(dst, c.data, len);
    total += len;
  }

  return total;
}

size_t block_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  bool direct = flags & O_DIRECT;
  bool sync = flags & O_SYNC;
  if (len == 0)
    return 0;

  size_t total = 0;
  size_t sector = offset / 512;
  size_t soff = offset % 512;
  auto *src = (const char*) buf;

  // Partial first sector.
  if (soff > 0) {
    auto &c = load_sector(sector, direct);
    size_t sz = min(len, 512 - soff);
    memcpy(c.data + soff, src, sz);
    c.dirty = true;

    src += sz;
    len -= sz;
    total += sz;
    sector++;
  }

  // Whole sectors.
  while (len >= 512) {
    auto &c = cache[sector];
    memcpy(c.data, src, 512);
    c.valid = true;
    c.dirty = true;

    src += 512;
    len -= 512;
    total += 512;
    sector++;
  }

  // Last partial sector.
  if (len > 0) {
    auto &c = load_sector(sector, direct);
    memcpy(c.data, src, len);
    c.dirty = true;

    total += len;
  }

  if (direct || sync)
    flush();

  return total;
}

void block_inode::flush() {
  for (const auto &[sector, c] : cache) {
    // Note we can't capture reference directly, since the pair is temporarily constructed.
    if (c.dirty)
      flush_sector(sector);
  }
}

devroot::devroot(class fs *fs) : inode_impl(fs, 0, 0) {
  lnkcnt = 2;
}

devfs::devfs() {
  auto node = new devroot(this);
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