#include "procfs.h"
#include "../proc/schedule.h"

namespace os::proc {

ssize_t filesystems::read(size_t offset, void *buf, size_t len, int) {
  auto content = string("\n").join(vfs::recorded_fs()) + "\n";
  if (offset >= content.size())
    return 0;
  size_t l = min(len, content.size() - offset);
  memcpy(buf, content.c_str() + offset, l);
  meta.atime = now();
  return l;
}

inode *process::lookup(const string &name) {
  if (name == "exe")
    return pcb->execpath.size() ? new link(fs, pcb->execpath) : nullptr;
  
  printk("process: unknown name: %s\n", name.c_str());
  return nullptr;
}

vector<inode::item> process::list() {
  vector<inode::item> result;
  if (pcb->execpath.size())
    result.push_back({ 0, "exe", File });
  return result;
}

}

namespace os {

procroot::procroot(class fs *fs):
  inode_impl(fs, 0, 0, 0555, Dir), filesystems(new proc::filesystems(fs)) {}

inode *procroot::lookup(const string &name) {
  meta.atime = now();
  if (name == "filesystems")
    return filesystems;
  if (name == "self") {
    int pid = active()->pcb->pid;
    if (pnodes.count(pid))
      return pnodes[pid];

    return pnodes[pid] = new proc::process(fs, (*pidmap)[pid]);
  }
  
  // Try convert the string into number.
  unsigned long res = 0; unsigned i = 0;
  const char *str = name.c_str();
  while (str[i] && str[i] <= '9' && str[i] >= '0') {
    res = res * 10 + str[i] - '0';
    i++;
  }
  // Now the entire string is a number.
  if (name.size() <= 10 && i == name.size() && res <= 2147483647) {
    int pid = (int) res;
    if (!pidmap->count(pid))
      return nullptr;
    if (pnodes.count(pid))
      return pnodes[pid];

    return pnodes[pid] = new proc::process(fs, (*pidmap)[pid]);
  }

  printk("procroot: unknown name: %s\n", name.c_str());
  return nullptr;
}

vector<inode::item> procroot::list() {
  meta.atime = now();
  vector<item> result;
  result.push_back({ filesystems->inum(), "filesystems", File });
  result.push_back({ active()->pcb->pid, "self", Dir });
  char buf[13];
  for (auto [pid, _] : *pidmap) {
    itoa(pid, buf, 10);
    result.push_back({ pid, buf, Dir });
  }
  return result;
}

procfs::procfs() {
  auto node = new procroot(this);
  root = new dentry("", node, nullptr);
}

expected<fs*> procfs_creator(const char *) {
  return new procfs;
}

}
