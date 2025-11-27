#include "ext2.h"
#include "../driver/virtio/virtio.h"

namespace {

using namespace os;

}

namespace os {

static_storage<ext2> ext2fs;

// size_t ext2_inode::read(size_t offset, void *buf, size_t len) {
  
// }

// Note that read() is synchronous on boot, but asynchronous in user processes.
ext2::ext2() {
  disk = virtio::get(0);
  char block[512];
  disk->read(2, block);
  memcpy(&superblock, block, sizeof(struct superblock));
  // Leave ext2 in an uninitialized state.
  if (superblock.magic != 0xef53)
    return;

  printk("version = %d.%d\n", superblock.ver_major, superblock.ver_minor);
  block_size = 1 << (10 + superblock.blocksz);
  auto group_count = (superblock.total_blocks + superblock.group_sz_blocks - 1) / superblock.group_sz_blocks;

  int gdt_start = (block_size == 1024) ? 2 : 1;
  gdt.resize(group_count);

  auto gdt_len = roundup(group_count * sizeof(block_group), block_size);
  char *buf = new char[gdt_len];
  for (unsigned i = 0; i < gdt_len / 512; i++)
    disk->read(gdt_start + i, buf + i * 512);
  memcpy(gdt.data(), buf, gdt_len);
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

void mount_ext2() {
  ext2fs.construct();
  if (!ext2fs.valid())
    panic("cannot mount ext2fs");
}

}
