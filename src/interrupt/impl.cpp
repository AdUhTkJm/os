#include "../fs/vfs.h"
#include "../proc/schedule.h"

namespace os::detail {

using namespace os;

int mount(const char *src, const char *tgt, const char *fsty, unsigned long flags) {
  auto vfs = active()->pcb->vfs;
  auto maybe_mntpoint = vfs->lookup(tgt);
  if (!maybe_mntpoint)
    return -maybe_mntpoint;

  dentry *mntpoint = *maybe_mntpoint;
  if (mntpoint->node->type != inode::Dir)
    return -ENOTDIR;
  // This place is mounted.
  if (mntpoint->mnt)
    return -EBUSY;

  if (flags & MS_MOVE) {
    auto source = vfs->lookup(src);
    if (!source)
      return -ENOENT;
    vfs::move_mount(*source, mntpoint);
    return 0;
  }

  expected<class fs*> fs = vfs->get(fsty, src);
  if (!fs)
    return fs;

  vfs::mount(mntpoint, (*fs)->root);
  return 0;
}

// For details, see https://linux.die.net/man/2/fcntl
int fcntl(int fd, int ty, int arg) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  if (!pcb->ftbl->count(fd))
    return -EBADF;
  switch (ty) {
  case F_SETFD:
    pcb->ftbl->set_desc(fd, arg);
    return 0;
  case F_GETFD:
    return *pcb->ftbl->get_desc(fd);
  default:
    return -EINVAL;
  }
}

int mprotect(unsigned long start, unsigned long len, int prot) {
  auto tcb = active();

  if (len == 0)
    return -EINVAL;
  start = rounddown<PAGE_SIZE>(start);
  auto finish = roundup<PAGE_SIZE>(start + len);
  auto pcb = tcb->pcb;
  if (!pcb->vma.has(start) || !pcb->vma.has(finish))
    return -ENOMEM;

  auto &vmas = pcb->vma;
  size_t begin = vmas.find(start);
  // Check memory contiguity.
  for (auto i = begin; i < vmas.size() && vmas[i].end < finish; i++) {
    if (vmas[i].end != vmas[i + 1].begin) 
      return -ENOMEM;
  }

  // Split the first VMA if needed at `start`. Now we're starting from the VMA to the right,
  // i.e. from `start` to original `vma.end`.
  if (vmas[begin].begin < start)
    vmas.split_at(begin++, start);

  // Note we don't include `finish` when we're mapping.
  size_t end = vmas.find(finish - 1);

  // Split the last VMA if needed at `end`. We don't need to update `end`, because this time
  // the split VMA is to the left.
  if (vmas[end].begin <= finish && finish < vmas[end].end)
    vmas.split_at(end, finish);

  // Now all VMAs that need changing are exactly those with indices in [begin, end].
  // Note this is inclusive on both ends.
  for (size_t i = begin; i <= end; ++i)
    vmas[i].prot = prot;
  
  // Remap existing memory.
  for (char *p = (char*) start; p != (char*) finish; p += PAGE_SIZE) {
    auto flags = pte_flags(p);
    if (flags == -1)
      continue;

    auto pa = to_pa(p);
    if (prot & PROT_EXEC) flags |= PTE_X;
    if (prot & PROT_READ) flags |= PTE_R;
    if (prot & PROT_WRITE) flags |= PTE_W;
    os::pmap(pa, (va_t) p, MAP_4KB, flags);
  }
  return 0;
}

}
