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

// We do want to use string-plus-int here. Moreover, gcc does not know these pragmas.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstring-plus-int"
#endif

ssize_t pid::oom_score_adj::read(size_t offset, void *buf, size_t len, int) {
  char buffer[10];
  itoa(value, buffer, 10);
  size_t buflen = strlen(buffer);
  buffer[buflen++] = '\n';
  buffer[buflen] = '\0';
  if (offset >= buflen)
    return 0;
  
  size_t l = min(len, buflen - offset);
  memcpy(buf, buffer + offset, l);
  return l;
}

ssize_t pid::oom_score_adj::write(size_t offset, const void *buf, size_t len, int) {
  if (offset != 0)
    return -EINVAL;
  // The maximum length possible is the string "-1000\n".
  if (len > 6)
    return -EINVAL;

  // Parse the integer.
  auto str = (const char *) buf;
  int res = 0, sign = 1; size_t i = 0;
  if (*str == '-') {
    sign = -1;
    i++;
  }
  for (; i < len && str[i] && str[i] <= '9' && str[i] >= '0'; i++)
    res = res * 10 + str[i] - '0';
  res *= sign;

  if (i == 0)
    return -EINVAL;
  if (i < len && str[i] == '\n')
    i++;

  value = res;
  return i;
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

ssize_t pid::mounts::read(size_t offset, void *buf, size_t len, int) {
  string value;
  for (const auto &data : *vfs::mounted) {
    value += data.device;
    value += " ";
    value += data.mntpoint;
    value += " "; 
    value += data.fstype;
    value += " ";
    value += data.prot & PROT_WRITE ? "rw" : "ro";
    value += " 0 0\n";
  }
  if (offset >= value.size())
    return 0;

  size_t l = min(value.size() - offset, len);
  memcpy(buf, value.c_str() + offset, l);
  return l;
}

process::process(class fs *fs, inode *parent, pcb_t *pcb): inode_impl(fs, 0, 0, 0666, Dir), pcb(pcb), parent(parent),
  exe(new link(fs, pcb->execpath)), oom(new pid::oom_score_adj(fs, pcb)), mounts(new pid::mounts(fs, pcb)) {
  exe->linked();
  oom->linked();
  mounts->linked();
}

process::~process() {
  exe->unlinked();
  oom->unlinked();
  mounts->unlinked();
}

inode *process::lookup(const string &name) {
  if (name == "exe")
    return exe;
  if (name == "oom_score_adj")
    return oom;
  if (name == "mounts")
    return mounts;
  
  printk("process: unknown name: %s\n", name.c_str());
  return nullptr;
}

vector<inode::item> process::list() {
  vector<inode::item> result;
  result.push_back({ inum(), ".", Dir });
  result.push_back({ parent->inum(), "..", Dir });
  if (pcb->execpath.size())
    result.push_back({ exe->inum(), "exe", File });
  result.push_back({ oom->inum(), "oom_score_adj", File });
  result.push_back({ mounts->inum(), "mounts", File });
  return result;
}

ssize_t meminfo::read(size_t offset, void *buf, size_t len, int) {
  // TODO: What about a string stream?
  string value("MemTotal: ");
  value += ptotal() * 4; // This is returned in pages (4 kb).
  value += " kB\n";

  value += "MemFree: ";
  value += pavail() * 4;
  value += " kB\n";

  value += "MemAvail: ";
  value += pavail() * 4;
  value += " kB\n";

  value += "Cached: ";
  value += 0;
  value += " kB\n";

  value += "SwapTotal: ";
  value += 0;
  value += " kB\n";

  value += "SwapFree: ";
  value += 0;
  value += " kB\n";

  if (offset >= value.size())
    return 0;
  auto l = min(value.size() - offset, len);
  memcpy(buf, value.c_str() + offset, l);
  return l;
};

ssize_t stat::read(size_t offset, void *buf, size_t len, int) {
  static const char *value =
R"(cpu 0 0 0 0 0 0 0 0 0 0
cpu0 0 0 0 0 0 0 0 0 0 0
intr 0
ctxt 0
btime 0
processes 1
procs_running 1
procs_blocked 0
)";
  static const size_t vlen = strlen(value);
  if (offset >= vlen)
    return 0;
  auto l = min(vlen - offset, len);
  memcpy(buf, value + offset, l);
  return vlen;
}

sys_inode::sys_inode(class fs *fs, inode *parent): inode_impl(fs, 0, 0, 0666, Dir), kernel(new sys::kernel(fs, this)), parent(parent) {
  kernel->linked();
  parent->linked();
}

sys_inode::~sys_inode() {
  kernel->unlinked();
  parent->unlinked();
}

vector<inode::item> sys_inode::list() {
  vector<item> result;
  result.push_back({ inum(), ".", Dir });
  result.push_back({ parent->inum(), "..", Dir });
  result.push_back({ kernel->inum(), "kernel", Dir });
  return result;
}

inode *sys_inode::lookup(const string &name) {
  if (name == "kernel")
    return kernel;
  return nullptr;
}

sys::kernel::kernel(class fs *fs, inode *parent):
  inode_impl(fs, 0, 0, 0666, Dir), tainted(new class tainted(fs)), parent(parent) {
  parent->linked();
  tainted->linked();
}

sys::kernel::~kernel() {
  parent->unlinked();
  tainted->unlinked();
}

vector<inode::item> sys::kernel::list() {
  vector<item> result;
  result.push_back({ inum(), ".", Dir });
  result.push_back({ parent->inum(), "..", Dir });
  result.push_back({ tainted->inum(), "tainted", File });
  return result;
}

inode *sys::kernel::lookup(const string &name) {
  if (name == "tainted")
    return tainted;

  return nullptr;
}

ssize_t sys::tainted::read(size_t offset, void *buf, size_t len, int) {
  char buffer[20];
  itoa(taint, buffer, 10);
  auto size = strlen(buffer);
  // Append an '\n'.
  buffer[size++] = '\n';
  buffer[size] = 0;

  if (offset >= size)
    return 0;

  auto l = min(size - offset, len);
  memcpy(buf, buffer + offset, l);
  return l;
}

}

namespace os {

procroot::procroot(class fs *fs):
  inode_impl(fs, 0, 0, 0555, Dir), filesystems(new proc::filesystems(fs)), meminfo(new proc::meminfo(fs)),
  stat(new proc::stat(fs)), sys(new proc::sys_inode(fs, this)), mounts(new proc::link(fs, "self/mounts")) {
  filesystems->linked();
  meminfo->linked();
  stat->linked();
  sys->linked();
  mounts->linked();
}

procroot::~procroot() {
  filesystems->unlinked();
  meminfo->unlinked();
  stat->unlinked();
  sys->unlinked();
  mounts->unlinked();
}

inode *procroot::lookup(const string &name) {
  meta.atime = now();
  if (name == "filesystems")
    return filesystems;
  if (name == "self") {
    int pid = active()->pcb->pid;
    if (pnodes.count(pid))
      return pnodes[pid];

    return pnodes[pid] = new proc::process(fs, this, (*pidmap)[pid]);
  }
  if (name == "meminfo")
    return meminfo;
  if (name == "stat")
    return stat;
  if (name == "sys")
    return sys;
  if (name == "mounts")
    return mounts;
  
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

    return pnodes[pid] = new proc::process(fs, this, (*pidmap)[pid]);
  }

  printk("procroot: unknown name: %s\n", name.c_str());
  return nullptr;
}

vector<inode::item> procroot::list() {
  meta.atime = now();
  vector<item> result;
  result.push_back({ inum(), ".", Dir });
  result.push_back({ filesystems->inum(), "filesystems", File });
  result.push_back({ meminfo->inum(), "meminfo", File });
  result.push_back({ stat->inum(), "stat", File });
  result.push_back({ mounts->inum(), "mounts", File });
  result.push_back({ sys->inum(), "sys", Dir });
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
