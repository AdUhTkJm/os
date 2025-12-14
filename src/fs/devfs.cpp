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
      wait.push_back(active());
      if (suspend() != 0)
        return i == 0 ? -EINTR : i;
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

short console_inode::poll(unsigned short event) {
  bool out = event & POLLOUT, in = event & POLLIN;
  auto result = 0;
  if (in && !console_input_buf->empty())
    result |= POLLIN;
  if (out)
    result |= POLLOUT;
  return result;
}

void console_inode::wake_read() {
  scheduler.wakeup_all(lock, wait);
}

void console_inode::wait_on_read() {
  wait.push_back(active());
}

block_inode::cached_sector &block_inode::load_sector(unsigned sector, bool force_reload) {
  auto &c = cache[sector];
  if (c.valid && !force_reload)
    return c;
  
  if (dev->read(sector, c.data) != 0)
    panic("block device: read failed");
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
    sector++;
  }

  // Full sectors.
  while (len >= 512) {
    auto &c = load_sector(sector, direct);
    memcpy(dst, c.data, 512);

    dst += 512;
    len -= 512;
    sector++;
  }

  // Last partial sector.
  if (len > 0) {
    auto &c = load_sector(sector, direct);
    memcpy(dst, c.data, len);
    dst += len;
  }

  return dst - (char *) buf;
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

tty_inode::tty_inode(console_inode *console): inode_impl(devfs.get(), /*uid=*/0, /*gid=*/0), tty(console) {

}

// Note we don't need to read the entire amount of `len`.
// We only need to guarantee we don't read more than `len`;
// for terminals, we should return whenever a newline occurs.
size_t tty_inode::read(size_t, void *buf, size_t len, int) {
  // TODO: noblock?
  if (line.size() == 0)
    line = tty.readline();

  auto l = min(len, line.size());
  memcpy(buf, line.c_str(), l);
  line = line.substr(l);
  return l;
}

size_t tty_inode::write(size_t, const void *buf, size_t len, int) {
  tty.write((const char*) buf, len);
  return len;
}

short tty_inode::poll(unsigned short event) {
  if (line.size() == 0)
    return tty.console->poll(event);

  // We still have something to read.
  short result = 0;
  if (event & POLLIN)
    result |= POLLIN;
  if (event & POLLOUT)
    result |= POLLOUT;
  return result;
}

void tty_inode::wake_read() {
  tty.console->wake_read();
}

void tty_inode::wait_on_read() {
  tty.console->wait_on_read();
}

devroot::devroot(class fs *fs) : inode_impl(fs, 0, 0) {
  type = Dir;
  lnkcnt = 2;
}

devfs::devfs() {
  auto node = new devroot(this);
  root = new dentry("dev", node, nullptr);
}

void mount_dev() {
  auto droot = devfs->root;
  auto root = cast<devroot>(droot->node);
  auto tcb = active();
  auto pcb = tcb->pcb;
  
  // console is initialized in PLIC handler.
  auto console = &*os::console;
  root->record("console", console);
  auto dentry = pcb->vfs->lookup("/dev");
  if (!dentry)
    panic("devfs: cannot find /dev");

  // Create a tty.
  auto tty = new (permanent) tty_inode(console);
  root->record("tty", tty);
  
  vfs::mount(*dentry, droot);
}

}
