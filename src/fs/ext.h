#ifndef EXT2_H
#define EXT2_H

#include "vfs.h"
#include "../driver/virtio/virtio.h"
#include "../utils/stl/crc.h"

/*
For ext2 format, see:
https://wiki.osdev.org/Ext2
*/

#define EXT4_INODE_EXTENTS  0x00080000

namespace os {

class ext_inode : public os::inode_impl<ext_inode> {
  struct meta {
    uint16_t type;  // file type + permission
    uint16_t uid;
    uint32_t sz;
    uint32_t atime; // atime
    uint32_t ctime; // ctime
    uint32_t mtime; // mtime
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
    uint32_t acl; // When 64-bit is enabled, this is higher 32 bits of file size.
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

  struct extent_header {
    uint16_t magic;
    uint16_t entries;
    uint16_t max;
    uint16_t depth;
    uint32_t gen;
  };

  struct extent {
    uint32_t logical;
    uint16_t len;
    uint16_t phys_hi;
    uint32_t phys_lo;
  };

  struct extent_idx {
    uint32_t logical;
    uint32_t leaf_lo;
    uint16_t leaf_hi;
    uint16_t __pad;
  };

  friend class ext;
  unique_ptr<char[]> read_block(unsigned long block);
  void write_block(unsigned long block, const char *data);
  int flags;

  // Finds the block number of this byte.
  // There might be some sparse holes, which would mean zero.
  size_t locate_ext2(size_t byte);
  size_t locate_ext4(size_t byte);

  size_t locate(size_t byte);

  // Set the index'th block in the pointer table to value `block`.
  int set_pointer_ext2(unsigned index, size_t value);
  int set_pointer_ext4(unsigned index, size_t value);

  int set_pointer(unsigned index, size_t value);

  vector<unsigned long> find_path(unsigned index);
  // Split node at level `level`, by inserting an extent `ext` into it.
  expected<extent_idx> split(const vector<unsigned long> &path, int level, int pos, const extent &ext);
  int insert_extent(const vector<unsigned long> &path, int level, int pos, const extent &ext);

  unsigned insertion_pos(extent_idx *indices, unsigned cnt, size_t logical);

  // Adds a directory entry.
  int add_dirent(const string &name, unsigned inum, unsigned char type);

  unsigned crc(const extent_header *extent_block) const;
public:
  using inode_impl::inode_impl;
  ext_inode(class fs *fs, const struct meta &meta, long inum);
  ext_inode(class fs *fs, long inum);

  enum ftypeflags : uint16_t {
    FIFO = 0x1000, CharDevice = 0x2000, Directory = 0x4000,
    BlockDevice = 0x6000, File = 0x8000, SymLink = 0xA000,
    Socket = 0xC000, Bad = 0xFFFF
  };
  static ftypeflags fromtype(inode::filetype ty);
  static inode::filetype totype(ftypeflags ty);
  static int to_dirent_type(inode::filetype ty);

  size_t read(size_t offset, void *buf, size_t len, int flags) override;
  size_t write(size_t offset, const void *buf, size_t len, int flags) override;
  int create(const string &name, filetype ty, int mode) override;
  int unlink(const string &name) override;
  inode *lookup(const string &name) override;
  vector<item> list() override;
  optional<string> readlink() override;
  inode::meta get_meta() override;
  void set_meta(const inode::meta &meta) override;

  size_t size() const override { return meta.sz; }
  long inum() const override { return _inum; }
};

class ext : public fs {
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
    uint16_t gdt_resv;
    char journal_uuid[16];
    uint32_t journal_inode;
    uint32_t journal_devnum;
    uint32_t orphan;
    char hashseed[16];
    uint8_t  hashalg;
    uint8_t  journal;
    uint16_t gd_size;
    uint32_t mount_opts;
    uint32_t metagroup;
    uint32_t fs_ctime;
    char backup[68];
    char __64bit_start[0];
    uint32_t total_blocks_hi;
    uint32_t block_su_hi;
    uint32_t free_blocks_hi;
    uint16_t min_inode_sz;
    uint16_t min_inode_resv_sz;
  } superblock;
  
  struct block_group {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t flags;
    uint32_t snapshot;
    uint16_t block_bitmap_crc;
    uint16_t inode_bitmap_crc;
    uint16_t never_used_inodes; // A hint, only for speed.
    uint16_t crc;

    char __64bit_start[0];
    uint32_t block_bitmap_hi;
    uint32_t inode_bitmap_hi;
    uint32_t inode_table_hi;
    uint16_t free_blocks_count_hi;
    uint16_t free_inodes_count_hi;
    uint16_t used_dirs_count_hi;
    uint16_t never_used_inodes_hi;
    uint32_t snapshot_hi;
    uint16_t block_bitmap_crc_hi;
    uint16_t inode_bitmap_crc_hi;
    uint32_t _resv;
  };

  // The size of each block, in bytes.
  unsigned blksz;
  // The size of group descriptor, in bytes. When 64-bit mode is not enabled, this is 32.
  unsigned gdsz = 32;
  // The group descriptor table.
  os::vector<block_group> gdt;
  inode *device;
  os::crc32c<0x1edc6f41> crc;
  // This cannot become LRU; we hope each inode file has a single instance in memory.
  os::hashmap<size_t, ext_inode*> nodecache;

  // Configuration.
  bool extent;
  bool fs_64;
  bool size_64;
  bool do_crc;

  void update_superblock();
  void update_block_group(int group_id);
  void update_meta(ext_inode *node);

  // Create an inode in memory from an existing file, located at `inum`.
  ext_inode *read_from_inum(size_t inum);

  // Calculate byte offset from beginning, given a block number.
  size_t offset(size_t blk);

  // Search for an unoccupied inode/block.
  ext_inode *search_inode(int id, block_group &gd);
  size_t search_block(int id, block_group &gd);

  size_t read_64(uint32_t lo, uint32_t hi);
  int write_64(size_t v, uint32_t &lo, uint32_t &hi);
  size_t read_32(uint16_t lo, uint16_t hi);
  int write_32(size_t v, uint16_t &lo, uint16_t &hi);
  
  // Allocate a new block.
  size_t balloc();
  void free_inode(size_t inum);
  void free_block(size_t block);
  void free_blocks(ext_inode *node, size_t block, int level);
  friend class ext_inode;
public:
  ext(inode *device);
  ext_inode *get() override;
  void erase(inode*) override;
  bool has_backup() override { return true; }
  void sync() override;

  bool valid() { return superblock.magic == 0xef53; }
};

expected<fs*> ext_creator(const char *src);

}

#endif
