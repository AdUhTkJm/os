#include "ext2.h"
#include "../proc/schedule.h"

namespace {

using namespace os;

}

namespace os {

static_storage<ext2> ext2fs;

// size_t ext2_inode::read(size_t offset, void *buf, size_t len) {
  
// }

// Note that read() is synchronous on boot, but asynchronous in user processes.
ext2::ext2(inode *device): device(device) {
  constexpr auto sbsz = sizeof(struct superblock);
  char block[sbsz];
  device->read(1024, block, sbsz, 0);
  memcpy(&superblock, block, sbsz);
  // Leave ext2 in an uninitialized state.
  if (superblock.magic != 0xef53)
    return;

  printk("version = %d.%d\n", superblock.ver_major, superblock.ver_minor);
  block_size = 1 << (10 + superblock.blocksz);
  auto group_count = (superblock.total_blocks + superblock.group_sz_blocks - 1) / superblock.group_sz_blocks;

  int gdt_start = (block_size == 1024) ? 2 : 1;
  gdt.resize(group_count);

  size_t len = group_count * sizeof(block_group);
  char *buf = new char[len];
  device->read(gdt_start, buf, len, 0);
  memcpy(gdt.data(), buf, len);
  delete[] buf;
  printk("ext2 initialized: groups = %d, block_size = %d\n", group_count, block_size);
}

ext2_inode *ext2::get() {
  assert(false);
}

void ext2::erase(inode *n) {
  // auto node = cast<ext2_inode>(n);
  (void) n;
}

expected<fs*> ext2_creator(const char *src) {
  auto pcb = scheduler.active;
  int fd = pcb->open_file(src, 0);
  if (fd < 0)
    return -EBADF;
  inode *node = pcb->ftbl[fd]->node;
  if (node->type != inode::BlockDevice)
    return -ENOTBLK;
  return new ext2(node);
}

}
