#ifndef EXT2_H
#define EXT2_H

#include "vfs.h"
#include "../driver/virtio/virtio.h"

/*
For ext2 format, see:
https://wiki.osdev.org/Ext2
*/

namespace os {

class ext2_inode : public os::inode_impl<ext2_inode> {
  struct meta {
    uint16_t type; // file type + permission
    uint16_t uid;
    uint32_t sz;
    uint32_t last_access_time;
    uint32_t create_time;
    uint32_t last_write_time;
    uint32_t delete_time;
    uint16_t gid;
    uint16_t refcnt;
    uint32_t sectors; // The number of sectors used by this file.
    uint32_t flags;
    uint32_t _resv0; // unused
    uint32_t directptr[12];
    uint32_t indirect1;
    uint32_t indirect2;
    uint32_t indirect3;
    uint32_t generation; // NFS only
    uint32_t acl;
    uint32_t upper_sz;
    uint32_t fragpos;
    uint8_t fragnum;
    uint8_t fragsz;
  } meta;

public:
  enum type : uint16_t {
    FIFO = 0x1000, CharDevice = 0x2000, Directory = 0x4000,
    BlockDevice = 0x6000, File = 0x8000, SymLink = 0xA000,
    Socket = 0xC000
  };

  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t offset, const void *buf, size_t len, int flags) override;
  result create(const string &name, filetype ty) override;
  inode *lookup(const string &name) override;
  vector<inode*> list() override;
};

class ext2 : public fs {
  struct superblock {
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t block_su;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t start_block_num;
    uint32_t blocksz; // log_2(size) - 10
    uint32_t fragsz;  // log_2(fragment size) - 10
    uint32_t group_sz_blocks; // Real value, in blocks
    uint32_t group_sz_frags;
    uint32_t group_sz_inodes;
    uint32_t last_mount_time;
    uint32_t last_write_time;
    // These are consistency check related. We don't do it for now.
    uint16_t _resv0;
    uint16_t _resv1;
    uint16_t magic; // 0xef53
    uint16_t state; // 1: clean; 2: erratic
    uint16_t error_behaviour; // 1: ignore; 2: remount; 3: kernel panic
    uint16_t ver_minor;
    uint32_t _resv2;
    uint32_t _resv3;
    uint32_t osid; // 0: Linux etc.
    uint32_t ver_major;
    uint16_t uid; // This uid and gid can use reserved blocks.
    uint16_t gid; // These relate to block_su.
  } superblock;
  
  struct block_group {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t _resv0;
    uint32_t _resv1[3];
  };

  virtio::block_device* disk;
  unsigned block_size;
  // The group descriptor table.
  os::vector<block_group> gdt;

  void update_superblock();
  void update_group_desc(uint32_t group_id);
public:
  ext2();
  ext2_inode *get() override;
  void erase(inode*) override;

  bool valid() { return superblock.magic == 0xef53; }
};

extern static_storage<ext2> ext2fs;
void mount_ext2();

}

#endif
