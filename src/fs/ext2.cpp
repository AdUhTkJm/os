#include "ext2.h"
#include "../proc/schedule.h"

namespace {

using namespace os;

}

namespace os {

static_storage<ext2> ext2fs;

// Create a node from existing data.
ext2_inode::ext2_inode(class fs *fs, const struct meta &meta, long inum):
  inode_impl(fs, meta.uid, meta.gid), meta(meta), _inum(inum) {
  // Copy data from disk into memory.
  type = totype(ftypeflags(meta.type & 0xf000));
  mode = meta.type & 0xfff;
}

// Create an empty node.
ext2_inode::ext2_inode(class fs *fs, long inum):
  inode_impl(fs, -1, -1), meta(), _inum(inum) { }

size_t ext2_inode::locate(size_t byte, int flags) {
  auto fs = static_cast<ext2*>(this->fs);
  auto block_size = fs->blksz;
  size_t cnt = byte / block_size;
  // Size of each array pointed by the indirect pointers.
  auto size = block_size / sizeof(int);
  if (cnt < 12)
    return meta.directptr[cnt];

  // We're in the range of single-indirect pointer.
  unique_ptr<unsigned[]> ptrs(new unsigned[block_size / sizeof(unsigned)]);
  cnt -= 12;
  if (cnt < size) {
    fs->device->read(fs->offset(meta.indirect1), ptrs.get(), block_size, flags);
    return ptrs[cnt];
  }

  // Second indirect pointer here.
  cnt -= size;
  if (cnt < size * size) {
    fs->device->read(fs->offset(meta.indirect2), ptrs.get(), block_size, flags);
    size_t b1 = ptrs[cnt / size];
    if (!b1)
      return 0;
    fs->device->read(fs->offset(b1), ptrs.get(), block_size, flags);
    return ptrs[cnt % size];
  }

  cnt -= size * size;
  if (cnt < size * size * size) {
    fs->device->read(fs->offset(meta.indirect3), ptrs.get(), block_size, flags);
    size_t b2 = ptrs[cnt / (size * size)];
    if (!b2)
      return 0;
    fs->device->read(fs->offset(b2), ptrs.get(), block_size, flags);
    size_t b1 = ptrs[(cnt / size) % size];
    if (!b1)
      return 0;
    fs->device->read(fs->offset(b1), ptrs.get(), block_size, flags);
    return ptrs[cnt % size];
  }

  // This is out of range.
  return -1;
}

int ext2_inode::set_pointer(unsigned index, unsigned value, int flags) {
  auto fs = static_cast<ext2*>(this->fs);
  unsigned block_size = fs->blksz;
  auto size = block_size / sizeof(int);
  if (index < 12) {
    meta.directptr[index] = value;
    return 0;
  }
  index -= 12;

  unique_ptr<char[]> zeroes_p(new char[block_size]);
  char *zeroes = zeroes_p.get();
  memset(zeroes, 0, block_size);
  if (index < size) {
    // Allocate the single-indirect block if it's not present.
    if (meta.indirect1 == 0) {
      unsigned b = fs->balloc();
      if (b == -1u)
        return -ENOSPC;

      meta.indirect1 = b;
      fs->device->write(fs->offset(b), zeroes, block_size, flags);
    }

    // Find the correct block. Each index is 4-byte long.
    unsigned b = fs->balloc();
    if (b == -1u)
      return -ENOSPC;
    fs->device->write(fs->offset(meta.indirect1) + index * sizeof(int), &b, sizeof(int), flags);
    return 0;
  }

  index -= size;
  if (index < size * size) {
    // Allocate the double-indirect block if it's not present.
    if (meta.indirect2 == 0) {
      unsigned b = fs->balloc();
      if (b == -1u)
        return -ENOSPC;

      meta.indirect2 = b;
      fs->device->write(fs->offset(b), zeroes, block_size, flags);
    }

    // Find the L1 block.
    unsigned l1;
    size_t l1pos = fs->offset(meta.indirect2) + (index / size) * sizeof(int);
    fs->device->read(l1pos, &l1, sizeof(int), flags);
    if (l1 == 0) {
      l1 = fs->balloc();
      if (l1 == -1u)
        return -ENOSPC;

      fs->device->write(fs->offset(l1), zeroes, block_size, flags);
      fs->device->write(l1pos, &l1, sizeof(int), flags);
    }

    // Find the L0 block.
    unsigned b = fs->balloc();
    if (b == -1u)
      return -ENOSPC;
    size_t l0pos = fs->offset(l1) + (index % size) * sizeof(int);
    fs->device->write(l0pos, &b, sizeof(int), flags);
    return 0;
  }

  index -= size * size;
  if (index < size * size * size) {
    // Allocate the triple-indirect block if it's not present.
    if (meta.indirect3 == 0) {
      unsigned b = fs->balloc();
      if (b == -1u)
        return -ENOSPC;

      meta.indirect3 = b;
      fs->device->write(fs->offset(b), zeroes, block_size, flags);
    }

    // Find the L2 block.
    unsigned l2;
    size_t l2pos = fs->offset(meta.indirect3) + (index / (size * size)) * sizeof(int);
    fs->device->read(l2pos, &l2, sizeof(int), flags);
    if (l2 == 0) {
      l2 = fs->balloc();
      if (l2 == -1u)
        return -ENOSPC;

      fs->device->write(fs->offset(l2), zeroes, block_size, flags);
      fs->device->write(l2pos, &l2, sizeof(int), flags);
    }

    // Find the L1 block.
    unsigned l1;
    size_t l1pos = fs->offset(meta.indirect3) + ((index / size) % size) * sizeof(int);
    fs->device->read(l1pos, &l1, sizeof(int), flags);
    if (l1 == 0) {
      l1 = fs->balloc();
      if (l1 == -1u)
        return -ENOSPC;

      fs->device->write(fs->offset(l1), zeroes, block_size, flags);
      fs->device->write(l1pos, &l1, sizeof(int), flags);
    }

    // Find the L0 block.
    unsigned b = fs->balloc();
    if (b == -1u)
      return -ENOSPC;
    size_t l0pos = fs->offset(l1) + (index % size) * sizeof(int);
    fs->device->write(l0pos, &b, sizeof(int), flags);
    return 0;
  }

  // Too large.
  return -ENOSPC;
}

int ext2_inode::add_dirent(const string &name, uint32_t inum, uint8_t type) {
  auto fs = static_cast<ext2*>(this->fs);
  size_t blksz = fs->blksz;

  for (size_t off = 0; off < meta.sz; off += blksz) {
    size_t b = locate(off, 0);
    if (!b)
      continue;

    unique_ptr<char[]> block = new char[blksz];
    fs->device->read(fs->offset(b), block.get(), blksz, 0);

    size_t pos = 0;
    while (pos < blksz) {
      // The old entry. We traverse through the entry list.
      auto *de = (direntry *)(block.get() + pos);
      size_t len = roundup<4>(sizeof(direntry) + de->namelen);

      // This is a full entry that occupies the rest of the block.
      // We shrink the previous entry and then create a new one.
      if (de->size - len >= roundup<4>(sizeof(direntry) + name.size())) {
        de->size = len;

        auto *ne = (direntry *)(block.get() + pos + len);
        ne->inum = inum;
        ne->namelen = name.size();
        ne->type = type;
        ne->size = blksz - (pos + len);
        memcpy(ne->name, name.c_str(), name.size());

        fs->device->write(fs->offset(b), block.get(), blksz, 0);
        fs->update_meta(this);
        return 0;
      }

      pos += de->size;
    }
  }

  // No space available. We must allocate a new block.
  unsigned newblk = fs->balloc();
  if (!newblk)
    return -ENOSPC;

  set_pointer(meta.sz / blksz, newblk, 0);

  unique_ptr<char[]> block = new char[blksz];
  memset(block.get(), 0, blksz);

  auto *entry = (direntry *) block.get();
  entry->inum = inum;
  entry->namelen = name.size();
  entry->type = type;
  entry->size = blksz;
  memcpy(entry->name, name.c_str(), name.size());

  fs->device->write(fs->offset(newblk), block.get(), blksz, 0);
  meta.sz += blksz;
  fs->update_meta(this);

  return 0;
}

size_t ext2_inode::read(size_t offset, void *buf, size_t len, int flags) {
  auto fs = static_cast<ext2*>(this->fs);

  size_t size = fs->blksz;
  size_t pos = offset;
  size_t end = min((size_t) meta.sz, offset + len);
  size_t read = 0;

  while (pos < end) {
    size_t b = locate(pos, flags);

    if (b == -1ul)
      return read;
    size_t chunk = min(size - pos % size, end - pos);

    char* p = (char*) buf + read;

    // Sparse hole.
    if (b == 0)
      memset(p, 0, chunk);
    else
      fs->device->read(fs->offset(b) + pos % size, p, chunk, flags);

    pos += chunk;
    read += chunk;
  }

  size_t time = now();
  meta.last_access_time = time;
  return read;
}

size_t ext2_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  if (len == 0)
    return 0;

  bool append = flags & O_APPEND;
  auto fs = static_cast<ext2*>(this->fs);
  offset = append ? meta.sz : offset;

  size_t size = fs->blksz;
  size_t pos = offset;
  size_t end = offset + len;
  size_t written = 0;

  while (pos < end) {
    size_t b = locate(pos, flags);
    if (b == -1ul)
      return written;
    
    if (b == 0) {
      unsigned newblk = fs->balloc();
      if (!newblk)
        break;

      // Attach block to inode.
      set_pointer(pos / fs->blksz, newblk, flags);

      // Zero the new block.
      unique_ptr<char[]> zero(new char[fs->blksz]);
      memset(zero.get(), 0, fs->blksz);
      fs->device->write(fs->offset(newblk), zero.get(), fs->blksz, 0);

      b = newblk;
    }
    size_t chunk = min(size - pos % size, end - pos);

    char* p = (char*) buf + written;

    fs->device->write(fs->offset(b) + pos % size, p, chunk, flags & ~O_APPEND);

    pos += chunk;
    written += chunk;
  }
  if (pos > meta.sz)
    meta.sz = pos;
  
  size_t time = now();
  meta.last_write_time = time;
  meta.last_access_time = time;
  fs->update_meta(this);
  return written;
}

ext2_inode::ftypeflags ext2_inode::fromtype(filetype ty) {
  switch (ty) {
  case filetype::File:
    return File;
  case filetype::Dir:
    return Directory;
  case filetype::Link:
    return SymLink;
  case filetype::BlockDevice:
    return BlockDevice;
  case filetype::Socket:
    return Socket;
  case filetype::CharDevice:
    return CharDevice;
  case filetype::FIFO:
    return FIFO;
  default:
    return Bad;
  }
}

int ext2_inode::to_dirent_type(filetype ty) {
  switch (ty) {
  case filetype::File:
    return 1;
  case filetype::Dir:
    return 2;
  case filetype::Link:
    return 7;
  case filetype::BlockDevice:
    return 4;
  case filetype::Socket:
    return 6;
  case filetype::CharDevice:
    return 3;
  case filetype::FIFO:
    return 5;
  default:
    return 0;
  }
}

ext2_inode::filetype ext2_inode::totype(ftypeflags ty) {
  switch (ty) {
  case File:
    return filetype::File;
  case Directory:
    return filetype::Dir;
  case SymLink:
    return filetype::Link;
  case BlockDevice:
    return filetype::BlockDevice;
  case Socket:
    return filetype::Socket;
  case CharDevice:
    return filetype::CharDevice;
  case FIFO:
    return filetype::FIFO;
  default:
    return filetype::Bad;
  }
}
int ext2_inode::create(const string &name, filetype ty, int mode) {
  if (type != Dir)
    return -ENOTDIR;

  auto fs = static_cast<ext2*>(this->fs);
  auto node = fs->get();

  auto pcb = active()->pcb;

  node->meta.type = fromtype(ty) | mode;
  node->meta.uid = pcb->uid;
  node->meta.gid = pcb->gid;
  node->meta.lnkcnt = (ty == Dir) ? 2 : 1;
  node->meta.create_time = now();
  fs->update_meta(node);

  // Metadata always get updated in add_dirent().
  if (auto ret = add_dirent(name, node->_inum, to_dirent_type(ty)); ret)
    return ret;

  if (ty == Dir) {
    node->add_dirent(".", node->_inum, to_dirent_type(Dir));
    node->add_dirent("..", _inum, to_dirent_type(Dir));
    meta.lnkcnt++;
  }
  return 0;
}

int ext2_inode::unlink(const string &name) {
  (void) name;
  return 0;
}

inode *ext2_inode::lookup(const string &name) {
  if (type != Dir)
    return nullptr;

  auto fs = static_cast<ext2*>(this->fs);

  // The basic structure is similar to list().
  for (unsigned pos = 0; pos < meta.sz;) {
    // Read the directory entry header.
    int block = locate(pos, 0);
    unsigned offset = block * fs->blksz + pos % fs->blksz;
    direntry entry;
    fs->device->read(offset, &entry, sizeof(entry), 0);
    
    if (entry.size < sizeof(entry))
      // Corrupt filesystem.
      return nullptr;

    if (entry.inum != 0) {
      unique_ptr<char[]> p(new char[entry.namelen + 1]);
      fs->device->read(offset + sizeof(entry), p.get(), entry.namelen, 0);
      p[entry.namelen] = '\0';
      // For string comparison, we must make sure `p` is a null-terminated string.
      if (name == p.get())
        return fs->read_from_inum(entry.inum);
    }
    pos += entry.size;
  }
  return nullptr;
}

static inode::filetype direntry_to_type(unsigned char ty) {
  switch (ty) {
  case 1:
    return inode::File;
  case 2:
    return inode::Dir;
  case 3:
    return inode::CharDevice;
  case 4:
    return inode::BlockDevice;
  case 5:
    return inode::FIFO;
  case 6:
    return inode::Socket;
  case 7:
    return inode::Link;
  default:
    return (inode::filetype) -1;
  }
}

vector<inode::item> ext2_inode::list() {
  if (type != Dir)
    return {};

  auto fs = static_cast<ext2*>(this->fs);
  vector<item> result;
  for (unsigned pos = 0; pos < meta.sz;) {
    // Read the directory entry header.
    int block = locate(pos, 0);
    unsigned offset = block * fs->blksz + pos % fs->blksz;
    direntry entry;
    fs->device->read(offset, &entry, sizeof(entry), 0);
    
    if (entry.size < sizeof(entry))
      // Corrupt filesystem.
      return result;

    if (entry.inum != 0) {
      auto p = new char[entry.namelen];
      fs->device->read(offset + sizeof(entry), p, entry.namelen, 0);
      auto name = string(p, entry.namelen);
      delete[] p;
      result.push_back({
        .inum = entry.inum, .name = name, .ty = direntry_to_type(entry.type)
      });
    }
    pos += entry.size;
  }
  return result;
}

optional<string> ext2_inode::readlink() {
  if (type != filetype::Link)
    return nullopt;
  // Data is directly stored in the directptr array, plus 3 indirect pointers.
  if (meta.sz <= 60) {
    char *str = (char*) &meta.directptr;
    return string(str, meta.sz);
  }
  // If the path is long, then it's the real content.
  char *content = new char[meta.sz];
  read(0, content, meta.sz, 0);
  auto value = string(content, meta.sz);
  delete[] content;
  return value;
}

// Currently we're assuming ext2 header starts at sector 2. This isn't always the case; read sectors 0 & 1 to know.
ext2::ext2(inode *device): device(device) {
  constexpr auto sbsz = sizeof(struct superblock);
  char block[sbsz];
  device->read(1024, block, sbsz, 0);
  memcpy(&superblock, block, sbsz);
  // Leave ext2 in an uninitialized state. In this state, this->root is nullptr,
  // so it's easy to detect an error.
  if (superblock.magic != 0xef53)
    return;

  if (superblock.ver_major < 1) {
    superblock.inode_size = 128;
    superblock.first_non_reserved = 11;
  }

  blksz = 1 << (10 + superblock.block_size);
  auto group_count = (superblock.total_blocks + superblock.block_per_group - 1) / superblock.block_per_group;

  int gdt_start = (blksz == 1024) ? 2 : 1;
  gdt.resize(group_count);

  size_t len = group_count * sizeof(block_group);
  char *buf = new char[len];
  device->read(gdt_start * blksz, buf, len, 0);
  memcpy(gdt.data(), buf, len);
  delete[] buf;

  // Root is always at inode 2.
  root = new dentry("", read_from_inum(2), nullptr);
  printk("ext2 version = %d.%d\n", superblock.ver_major, superblock.ver_minor);
}

size_t ext2::offset(size_t id) {
  return (id - superblock.start_block_num) * blksz;
}

void ext2::update_superblock() {
  device->write(1024, &superblock, sizeof(struct superblock), 0);
}

void ext2::update_block_group(int id) {
  int gdt_start = (blksz == 1024) ? 2 : 1;
  device->write(gdt_start * blksz + id * sizeof(block_group), &gdt[id], sizeof(block_group), 0);
}

ext2_inode *ext2::search_inode(int groupid, block_group &gd) {
  if (!gd.free_inodes_count)
    return nullptr;

  // Each group contains an inode bitmap. We read it.
  unique_ptr<char[]> bitmap(new char[blksz]);
  device->read(offset(gd.inode_bitmap), bitmap.get(), blksz, 0);

  // Search the Bitmap for the first free bit, 0.
  for (unsigned i = 0; i < blksz; i++) {
    unsigned char byte = bitmap[i];
    
    // 0 is free and 1 is occupied. Skip the fully occupied byte.
    if (byte == 0xFF)
      continue;
    // Don't touch reserved inodes.
    if (i * 8 + 7 < superblock.first_non_reserved)
      continue;

    for (int j = 0; j < 8; j++) {
      if (byte & (1 << j))
        continue;
      if (i * 8 + j < superblock.first_non_reserved)
        continue;
      
      unsigned inum = groupid * superblock.inodes_per_group + i * 8 + j + 1;
      bitmap[i] |= (1 << j);
      
      gd.free_inodes_count--;
      superblock.free_inodes--;
      
      // Write back changes.
      device->write(offset(gd.inode_bitmap), bitmap.get(), blksz, 0);
      update_block_group(groupid);
      update_superblock();

      return new ext2_inode(this, inum);
    }
  }
  return nullptr;
}

unsigned ext2::search_block(int groupid, block_group &gd) {
  if (!gd.free_blocks_count)
    return -1;

  unique_ptr<char[]> bitmap(new char[blksz]);
  device->read(offset(gd.block_bitmap), bitmap.get(), blksz, 0);

  // Search the Bitmap for the first free bit, 0.
  for (unsigned i = 0; i < blksz; i++) {
    unsigned char byte = bitmap[i];
    
    // 0 is free and 1 is occupied. Skip the fully occupied byte.
    if (byte == 0xFF)
      continue;

    for (int j = 0; j < 8; j++) {
      if (byte & (1 << j))
        continue;
      
      unsigned inum = groupid * superblock.inodes_per_group + i * 8 + j + 1;
      bitmap[i] |= (1 << j);
      
      gd.free_blocks_count--;
      superblock.free_blocks--;
      
      // Write back changes.
      device->write(offset(gd.block_bitmap), bitmap.get(), blksz, 0);
      update_block_group(groupid);
      update_superblock();

      return inum;
    }
  }
  return -1;
}

ext2_inode *ext2::get() {
  for (unsigned i = 0; i < gdt.size(); i++) {
    if (auto ret = search_inode(i, gdt[i]))
      return ret;
  }
  return nullptr;
}

unsigned ext2::balloc() {
  for (unsigned i = 0; i < gdt.size(); i++) {
    if (auto ret = search_block(i, gdt[i]))
      return ret;
  }
  return -1;
}

void ext2::erase(inode *n) {
  // auto node = cast<ext2_inode>(n);
  (void) n;
}

// These are a pair of functions that read/write metadata.

void ext2::update_meta(ext2_inode *node) {
  unsigned inum = node->_inum;
  unsigned group = (inum - 1) / superblock.inodes_per_group;
  unsigned index = (inum - 1) % superblock.inodes_per_group;

  const block_group &gd = gdt[group];

  size_t offset = gd.inode_table * blksz + index * superblock.inode_size;
  device->write(offset, &node->meta, superblock.inode_size, 0);
}

ext2_inode *ext2::read_from_inum(size_t inum) {
  auto group = (inum - 1) / superblock.inodes_per_group;
  auto index = (inum - 1) % superblock.inodes_per_group;

  const block_group &gd = gdt[group];
  // Compute byte offset.
  auto offset = gd.inode_table * blksz + index * superblock.inode_size;

  auto meta = (struct ext2_inode::meta*) vmalloc(superblock.inode_size);
  device->read(offset, meta, superblock.inode_size, 0);
  auto inode = new ext2_inode(this, *meta, inum);
  vfree(meta);
  return inode;
}

void ext2::sync() {
  auto dev = dyn_cast<block_inode>(device);
  if (!dev)
    panic("ext2: not a block inode");
  dev->flush();
}

expected<fs*> ext2_creator(const char *src) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  
  int fd = pcb->open_file(src, 0);
  if (fd < 0)
    return -EBADF;
  inode *node = pcb->ftbl->at(fd)->node();
  if (node->type != inode::BlockDevice)
    return -ENOTBLK;
  auto ext = new ext2(node);
  if (!ext->root)
    return -EINVAL;
  return ext;
}

}
