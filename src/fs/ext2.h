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
    uint16_t lnkcnt;
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
  long _inum;

  struct direntry {
    uint32_t inum;
    uint16_t size;
    uint8_t  namelen;
    uint8_t  type;
    char name[];
  };

  friend class ext2;
  // Finds the block number of this byte.
  // There might be some sparse holes, which would mean zero.
  size_t locate(size_t byte, int flags);

  // Set the index'th block in the pointer table to value `block`.
  result set_pointer(unsigned index, unsigned value, int flags);
public:
  using inode_impl::inode_impl;
  ext2_inode(class fs *fs, const struct meta &meta, long inum);
  ext2_inode(class fs *fs, long inum);

  enum ftypeflags : uint16_t {
    FIFO = 0x1000, CharDevice = 0x2000, Directory = 0x4000,
    BlockDevice = 0x6000, File = 0x8000, SymLink = 0xA000,
    Socket = 0xC000, Bad = 0xFFFF
  };
  static ftypeflags fromtype(inode::filetype ty);
  static inode::filetype totype(ftypeflags ty);

  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t offset, const void *buf, size_t len, int flags) override;
  int create(const string &name, filetype ty, int mode) override;
  int unlink(const string &name) override;
  inode *lookup(const string &name) override;
  vector<item> list() override;
  optional<string> readlink() override;

  size_t size() const override { return meta.sz; }
  long inum() const override { return _inum; }
};

class ext2 : public fs {
  struct superblock {
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t block_su;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t start_block_num;
    uint32_t block_size; // log_2(size) - 10
    uint32_t fragsz;  // log_2(fragment size) - 10
    uint32_t block_per_group;
    uint32_t flags_per_group;
    uint32_t inodes_per_group;
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
    uint32_t first_non_reserved;
    uint16_t inode_size; // In bytes.
    uint32_t optional_features;
    uint32_t required_features;
    uint32_t readonly_features;
    char fsid[16];
    char volume_name[16];
    char last_mounted_path[64];
    uint32_t compress_alg;
    uint8_t  file_prealloc_cnt;
    uint8_t  dir_prealloc_cnt;
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

  // The size of each block, in bytes.
  unsigned blksz;
  // The group descriptor table.
  os::vector<block_group> gdt;
  inode *device;

  void update_superblock();
  void update_block_group(int group_id);
  void update_meta(ext2_inode *node);

  // Create an inode in memory from an existing file, located at `inum`.
  ext2_inode *read_from_inum(size_t inum);

  // Calculate byte offset from beginning, given a block number.
  size_t offset(size_t blk);

  // Search for an unoccupied inode/block.
  ext2_inode *search_inode(int id, block_group &gd);
  unsigned search_block(int id, block_group &gd);
  
  // Allocate a new block.
  unsigned balloc();
  friend class ext2_inode;
public:
  ext2(inode *device);
  ext2_inode *get() override;
  void erase(inode*) override;
  bool has_backup() override { return true; }

  bool valid() { return superblock.magic == 0xef53; }
};

expected<fs*> ext2_creator(const char *src);

}

#endif
