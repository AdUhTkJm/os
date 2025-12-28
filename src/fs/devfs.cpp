#include "devfs.h"
#include "../driver/plic/plic.h"
#include "../driver/virtio/virtio.h"
#include "../proc/schedule.h"
#include "../utils/errorcode.h"

namespace os {

static_storage<console_inode> console;
static_storage<class devfs> devfs;

ssize_t console_inode::read(size_t offset, void *buf, size_t len, int flags) {
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
      if (suspend() != 0)
        return i == 0 ? -EINTR : i;
      lock.acquire();
      wait.finish(entry);

      c = console_input_buf->pop_front();
    }
    lock.release();
    p[i] = *c;
  }
  return len;
}

ssize_t console_inode::write(size_t offset, const void *buf, size_t len, int) {
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

void console_inode::prepare_read_wait(wait_entry &entry) {
  wait.prepare(entry);
}

void console_inode::finish_read_wait(wait_entry &entry) {
  wait.finish(entry);
}

block_inode::cached_sector *block_inode::load_page(unsigned page, bool force_reload) {
  auto c = cache.find(page);
  if (!c) {
    c = new cached_sector();
    c->key = page;
    c->data = (unsigned char *) as_va(pframe());
  } else if (!force_reload)
    return c;
  
  unsigned sector = page * 8;
  if (auto ret = dev->read(sector, c->data, 8); ret != 0)
    return printk("device: error code %d\n", ret), nullptr;
  active()->ruse.ru_inblock++;

  c->dirty = false;
  // TODO: use a wait queue instead, for concurrency issues.
  if (!cache.find(page))
    cache.insert(c);
  return c;
}

void block_inode::flush_page(unsigned page) {
  auto c = cache.find(page);
  if (!c || !c->dirty)
    return;
  dev->write(page, c->data, 8);
  active()->ruse.ru_oublock++;
  c->dirty = false;
}

#define LOAD_SECTOR(c, sector, direct) \
  auto cp = load_sector(sector, direct); \
  if (!cp) \
    return cp; \
  auto &c = **cp;

ssize_t block_inode::read(size_t offset, void *buf, size_t len, int flags) {
  size_t pos = 0;

  while (pos < len) {
    size_t cur = offset + pos;
    size_t off = cur % PAGE_SIZE;
    
    auto cp = load_page(cur / PAGE_SIZE, flags & O_DIRECT);
    if (!cp)
      return -EIO;

    size_t l = min(len - pos, PAGE_SIZE - off);
    memcpy((char *) buf + pos, cp->data + off, l);
    
    pos += l;
  }
  return pos;
}

ssize_t block_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  size_t pos = 0;

  while (pos < len) {
    size_t cur = offset + pos;
    size_t off = cur % PAGE_SIZE;
    size_t l = min(len - pos, PAGE_SIZE - off);

    // If writing an entire page, we don't need to load old data.
    bool full = (off == 0 && l == PAGE_SIZE);
    auto cp = load_page(cur / PAGE_SIZE, (flags & O_DIRECT) && !full);
    
    memcpy(cp->data + off, (char*) buf + pos, l);
    
    cp->dirty = true;
    pos += l;
  }

  if (flags & (O_DIRECT | O_SYNC))
    flush();
  return pos;
}

void block_inode::flush() {
  // TODO: enumerate an RB-tree!
}

void *block_inode::get_page(unsigned i) {
  return load_page(i)->data;
}

void block_inode::mark_dirty(unsigned i) {
  cache.find(i)->dirty = true;
}

tty_inode::tty_inode(console_inode *console): inode_impl(devfs.get(), 0, 0, 0666, File), tty(console) {}

// Note we don't need to read the entire amount of `len`.
// We only need to guarantee we don't read more than `len`;
// for terminals, we should return whenever a newline occurs.
ssize_t tty_inode::read(size_t, void *buf, size_t len, int) {
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

ssize_t tty_inode::write(size_t, const void *buf, size_t len, int) {
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

void tty_inode::prepare_read_wait(wait_entry &entry) {
  tty.console->prepare_read_wait(entry);
}

void tty_inode::finish_read_wait(wait_entry &entry) {
  tty.console->finish_read_wait(entry);
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
