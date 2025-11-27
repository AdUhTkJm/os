#include "devfs.h"
#include "../driver/plic/plic.h"
#include "../proc/schedule.h"
#include "../utils/errorcode.h"

namespace {

class devroot : public os::inode_impl<devroot> {
  os::hashmap<os::string, inode*> children;
public:
  using os::inode_impl<devroot>::inode_impl;
  size_t read(size_t, void *, size_t, int) override { return 0; }
  size_t write(size_t, const void *, size_t, int) override { return 0; }
  os::result create(const os::string &, os::inode::filetype) override { return os::result::failure; }
  os::inode *lookup(const os::string &name) override {
    return children[name];
  }
  os::vector<os::inode *> list() override {
    os::vector<os::inode*> result;
    result.reserve(children.size());
    for (auto [_, inode] : children)
      result.push_back(inode);
    return result;
  }

  // Special registration function.
  void record(const os::string &name, inode *node) {
    children[name] = node;
  }
};

}

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
  scheduler.wakeup(wait.front());
  wait.pop_front();
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
  
  vfs->mount("dev", dentry, root);
}

}