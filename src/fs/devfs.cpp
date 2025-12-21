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

  wait_entry entry;
  for (unsigned i = 0; i < len; i++) {
    optional<char> c = console_input_buf->pop_front();
    lock.acquire();
    while (!c) {
      if (!block) {
        lock.release();
        return i == 0 ? -EAGAIN : i;
      }

      wait.prepare(entry);
      lock.release();
      assert(detail::nested_irq == 0);
      if (suspend() != 0)
        return i == 0 ? -EINTR : i;
      lock.acquire();
      wait.finish(entry);

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
  wait.wake_all();
}

void console_inode::wait_on_read() {
  wait_entry entry;
  wait.prepare(entry);
}

block_inode::cached_sector &block_inode::load_sector(unsigned sector, bool force_reload) {
  auto &c = cache[sector];
  if (c.valid && !force_reload)
    return c;
  
  if (auto ret = dev->read(sector, c.data); ret != 0) {
    printk("read return: %d\n", ret);
    panic("block device: read failed");
  }
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
  for (const auto &[sector, c] : cache)
    // Note we can't capture reference directly, since the pair is temporarily constructed.
    // This `flush_sector` will always check dirtiness.
    flush_sector(sector);
}

tty_inode::tty_inode(console_inode *console): inode_impl(devfs.get(), 0, 0, 0666, File), tty(console) {}

// Note we don't need to read the entire amount of `len`.
// We only need to guarantee we don't read more than `len`;
// for terminals, we should return whenever a newline occurs.
size_t tty_inode::read(size_t, void *buf, size_t len, int) {
  // TODO: noblock?
  if (line.size() == 0) {
    line = tty.readline();
    // We read an EOF.
    if (line.size() == 0)
      return 0;
  }

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

null_inode::null_inode(): inode_impl(&*devfs, 0, 0, 0666, File) {}

devroot::devroot(class fs *fs) : inode_impl(fs, 0, 0, 0755, Dir) {}

inode *devroot::lookup(const string &name) {
  if (!children.count(name))
    return nullptr;
  return children[name];
}

vector<inode::item> devroot::list() {
  vector<item> result;
  result.reserve(children.size());
  for (auto [name, inode] : children)
    result.push_back({ .inum = (long) inode, .name = name, .ty = inode->type });
  return result;
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

  root->record("tty",  new (permanent) tty_inode(console));
  root->record("null", new (permanent) null_inode());
  root->record(".", root);
  
  vfs::mount(*dentry, droot);
}

}
