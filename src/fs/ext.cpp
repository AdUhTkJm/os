#include "ext.h"
#include "../proc/schedule.h"

namespace {

template<typename T>
void insert(T *arr, uint16_t &size, int pos, const T &ent) {
  for (int i = size; i >= pos; i++)
    arr[i + 1] = arr[i];
  arr[pos] = ent;
  size++;
}

template<typename T>
void remove_entry(T *arr, uint16_t &size, int pos) {
  for (int i = pos; i < size; i++)
    arr[i] = arr[i + 1];
  size--;
}

}

namespace os {

static_storage<ext> ext2fs;

// Create a node from existing data.
ext_inode::ext_inode(class fs *fs, const struct meta &meta, long inum):
  inode_impl(fs, meta.uid, meta.gid, meta.type & 0xfff, totype(ftypeflags(meta.type & 0xf000))), meta(meta), _inum(inum) {}

// Create an empty node.
ext_inode::ext_inode(class fs *fs, long inum):
  inode_impl(fs, -1, -1, 0000, filetype::Bad), meta(), _inum(inum) { }

size_t ext_inode::locate_ext2(size_t byte) {
  auto fs = static_cast<ext*>(this->fs);
  auto block_size = fs->blksz;
  size_t cnt = byte / block_size;
  // Size of each array pointed by the indirect pointers.
  const auto N = block_size / sizeof(int);
  if (cnt < 12)
    return meta.directptr[cnt];

  // We're in the range of single-indirect pointer.
  unsigned *ptrs;
  cnt -= 12;
  if (cnt < N) {
    ptrs = (unsigned *) read_block(meta.indirect1);
    return ptrs[cnt];
  }

  // Second indirect pointer here.
  cnt -= N;
  if (cnt < N * N) {
    ptrs = (unsigned *) read_block(meta.indirect2);
    size_t b1 = ptrs[cnt / N];
    if (!b1)
      return 0;
    ptrs = (unsigned *) read_block(b1);
    return ptrs[cnt % N];
  }

  cnt -= N * N;
  if (cnt < N * N * N) {
    ptrs = (unsigned *) read_block(meta.indirect3);
    size_t b2 = ptrs[cnt / (N * N)];
    if (!b2)
      return 0;
    ptrs = (unsigned *) read_block(b2);
    size_t b1 = ptrs[(cnt / N) % N];
    if (!b1)
      return 0;
    ptrs = (unsigned *) read_block(b1);
    return ptrs[cnt % N];
  }

  // This is out of range.
  return -1;
}

unsigned ext_inode::crc(const extent_header *extent_block) const {
  auto fs = static_cast<ext*>(this->fs);
  unsigned hash = fs->crc(fs->superblock.fsid, 16, 0xFFFFFFFF);

  hash = fs->crc(&_inum, 4, hash);
  hash = fs->crc(&meta.generation, 4, hash);
  return fs->crc(extent_block, fs->blksz - 12, hash);
}

char *ext_inode::read_block_mutable(unsigned long block) {
  auto fs = static_cast<ext*>(this->fs);
  return (char *) fs->device->get_page(fs->offset(block) / PAGE_SIZE);
}

const char *ext_inode::read_block(unsigned long block) {
  return read_block_mutable(block);
}

void ext_inode::write_block(unsigned long block, const char *data) {
  auto fs = static_cast<ext*>(this->fs);
  fs->device->write(fs->offset(block), data, fs->blksz, flags);
}

size_t ext_inode::locate_ext4(size_t byte) {
  auto fs = static_cast<ext*>(this->fs);
  uint32_t tgt = byte / fs->blksz;
  auto path = find_path(tgt);
  if (path.size() == 0)
    return 0;

  auto block = path.back();
  const extent_header *header;
  if (block != 0) {
    const char *p = read_block(block);
    if (!p)
      return 0;
    header = (const extent_header *) p;
  } else
    header = (const extent_header *) meta.directptr;
  extent* ext = (extent*) (header + 1);
  
  for (int i = 0; i < header->entries; i++) {
    // Target falls in the range of this extent.
    // Note that if length >= 0x8000, then this is an "uninitialized" fragment;
    // we should retrieve the real length by extracting the lower 15 bits.
    if (tgt >= ext[i].logical && tgt < (ext[i].logical + (ext[i].len & 0x7fff)))
      return ((unsigned long) ext[i].phys_hi << 32) + ext[i].phys_lo + tgt - ext[i].logical;
  }

  // A hole in the file.
  return 0;
}

unsigned ext_inode::insertion_pos(extent_idx *indices, unsigned cnt, size_t logical) {
  // TODO: binary search?
  for (int i = cnt - 1; i >= 0; i--) {
    if (indices[i].logical <= logical)
      return i + 1;
  }
  return -1;
}

size_t ext_inode::locate(size_t byte) {
  if (meta.flags & EXT4_INODE_EXTENTS)
    return locate_ext4(byte);
  return locate_ext2(byte);
}

int ext_inode::set_pointer_ext2(unsigned index, size_t value) {
  auto fs = static_cast<ext*>(this->fs);
  unsigned block_size = fs->blksz;
  const auto N = block_size / sizeof(int);
  if (index < 12) {
    meta.directptr[index] = value;
    return 0;
  }
  index -= 12;

  unique_ptr<char[]> zeroes_p(new char[block_size]);
  char *zeroes = zeroes_p.get();
  memset(zeroes, 0, block_size);
  if (index < N) {
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

  index -= N;
  if (index < N * N) {
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
    size_t l1pos = fs->offset(meta.indirect2) + (index / N) * sizeof(int);
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
    size_t l0pos = fs->offset(l1) + (index % N) * sizeof(int);
    fs->device->write(l0pos, &b, sizeof(int), flags);
    return 0;
  }

  index -= N * N;
  if (index < N * N * N) {
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
    size_t l2pos = fs->offset(meta.indirect3) + (index / (N * N)) * sizeof(int);
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
    size_t l1pos = fs->offset(meta.indirect3) + ((index / N) % N) * sizeof(int);
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
    size_t l0pos = fs->offset(l1) + (index % N) * sizeof(int);
    fs->device->write(l0pos, &b, sizeof(int), flags);
    return 0;
  }

  // Too large.
  return -ENOSPC;
}

vector<unsigned long> ext_inode::find_path(unsigned tgt) {
  vector<unsigned long> path;
  auto fs = static_cast<ext*>(this->fs);
  auto header = (extent_header *) meta.directptr;
  unique_ptr<char[]> buf(new char[fs->blksz]);
  uint64_t block = 0;
  int last_depth = -1;

  for (auto idx = (extent_idx *) (header + 1);;) {
    path.push_back(block);
    if (header->magic != 0xf30a)
      fs->on_corrupt();

    if (header->depth == 0)
      return path;
    
    if (header->depth - last_depth != 1 && path.size() > 1)
      fs->on_corrupt();
    last_depth = header->depth;

    // We must recurse.
    extent_idx *node = nullptr;

    // Find the index entry containing the target block.
    for (int i = 0; i < header->entries; ++i) {
      if (tgt >= idx[i].logical)
        node = idx + i;
      else break;
    }
    // This is a hole.
    if (!node)
      return { };

    // The new block still starts with a header, and followed by indices.
    block = ((unsigned long) node->leaf_hi << 32) + node->leaf_lo;
    if (block == 0)
      fs->on_corrupt();

    fs->device->read(fs->offset(block), buf.get(), fs->blksz, flags);
    header = (extent_header *) buf.get();
    idx = (extent_idx *) (header + 1);

    if (fs->do_crc) {
      auto crc = *(unsigned *)(buf.get() + fs->blksz - 12);
      auto computed = this->crc(header);
      printk("do crc: %d vs %d\n", crc, computed);
      if (crc != computed) {
        printk("ext: crc mismatch: %p != %p", crc, computed);
        fs->on_corrupt();
      }
    }
  }
}

expected<ext_inode::extent_idx> ext_inode::split(const vector<unsigned long> &path, int level, int pos, const extent &ext) {
  auto fs = static_cast<class ext*>(this->fs);
  assert(path[level] != 0);
  auto blockbuf = read_block(path[level]);
  if (!blockbuf)
    return -EIO;

  // NOTE: we cast out `const` here.
  auto old = (extent_header *) blockbuf;
  fs->device->mark_dirty(path[level]);

  uint64_t b = fs->balloc();
  if (b == -1ul)
    return -ENOSPC;

  unique_ptr<char[]> buf(new char[fs->blksz]);

  // Allocate a new header.
  auto h = (extent_header *) buf.get();
  // Leave some space for CRC.
  unsigned space = fs->blksz - sizeof(extent_header);
  int mid = old->entries / 2;
  if (fs->do_crc)
    space -= 12;

  h->magic = 0xf30a;
  h->max = space / sizeof(extent);
  h->depth = 0;
  h->entries = old->entries - mid;
  h->gen = old->gen;
  old->entries = mid;

  auto old_ext = (extent *) (old + 1);
  auto new_ext = (extent *) (h + 1);

  // Split half of entries into the new header.
  memcpy(new_ext, &old_ext[mid], (old->entries - mid) * sizeof(extent));

  // Calculate CRC for the new leaf block if required.
  if (fs->do_crc)
    *(unsigned*) ((char *) h + space) = crc(h);  

  // Insert the given entry, `ext`.
  if (pos < mid)
    insert(old_ext, old->entries, pos, ext);
  else
    insert(new_ext, h->entries, pos - mid, ext);

  // Write back.
  fs->device->write(fs->offset(path[level]), old, fs->blksz, 0);
  fs->device->write(fs->offset(b), h, fs->blksz, 0);

  // Now we must insert a new record into parent's index entry,
  // that points to the newly allocated extent.
  extent_idx out {
    .logical = new_ext[0].logical,
    .leaf_lo = (unsigned) b,
    .leaf_hi = (uint16_t) (b >> 32),
    .__pad = 0,
  };
  return out;
}

int ext_inode::insert_extent(const vector<unsigned long> &path, int level, int pos, const extent &ext) {
  // Split leaf.
  auto outp = split(path, level, pos, ext);
  if (!outp)
    return outp;
  auto out = *outp;

  // Insert the entry into parents.
  auto fs = (class ext *) this->fs;
  for (int i = int(path.size()) - 2; i >= 1; i--) {
    auto parent = path[i];
    auto buf = read_block_mutable(parent);
    if (!buf)
      return -EIO;

    auto header = (extent_header*) buf;
    auto indices = (extent_idx *) (header + 1);
    unsigned pos = insertion_pos(indices, header->entries, out.logical);

    if (header->entries != header->max) {
      insert(indices, header->entries, pos, out);
      fs->device->mark_dirty(parent);
      write_block(parent, buf);
      return 0;
    }

    // Now we must also split header.
    // Since the sizes and paddings are equal, we can reuse the same function.
    static_assert(sizeof(extent) == sizeof(extent_idx));
    // Remember that copy constructor of `expected` is disabled.
    auto r = split(path, i, pos, *(extent*) &out);
    if (!r)
      return r;
    out = *r;
  }
  
  auto header = (extent_header *) meta.directptr;
  // It doesn't matter whether this is extent_idx or extent. The `logical` field is at the same position.
  auto indices = (extent_idx *) (header + 1);

  if (header->entries != header->max) {
    insert(indices, header->entries, pos, out);
    return 0;
  }

  // If we reached here, we must split the root.
  auto block = fs->balloc();
  if (block == -1ul)
    return -ENOSPC;

  // We don't split the root into half, as it's too small; rather, we move all entries out.
  // Copy the contents into the new block.
  unique_ptr<char[]> buf(new char[fs->blksz]);
  auto h = (extent_header *) buf.get();
  unsigned space = fs->blksz - sizeof(extent_header);
  if (fs->do_crc)
    space -= 12;

  h->magic = 0xf30a;
  h->depth = header->depth;
  h->entries = header->entries;
  h->gen = header->gen;
  h->max = space / sizeof(extent);
  memcpy(h + 1, header + 1, 48);
  write_block(block, buf.get());

  // Update the root, i.e. inode's metadata. No write back now; wait for update_meta() later.
  extent_idx idx {
    .logical = indices->logical,
    .leaf_lo = (unsigned) block,
    .leaf_hi = (uint16_t) (block >> 32),
    .__pad = 0,
  };
  header->entries = 1;
  header->depth = h->depth + 1;
  memcpy(h + 1, &idx, sizeof(extent_idx));
  return 0;
}

int ext_inode::set_pointer_ext4(unsigned index, size_t value) {
  auto fs = static_cast<ext*>(this->fs);
  auto path = find_path(index);
  auto block = path.back();
  auto buf = read_block_mutable(block);
  if (!buf)
    return -EIO;

  auto header = (extent_header *) buf;
  auto exts = (extent *) (header + 1);

  assert(header->depth == 0);

  if (header->entries > 0) {
    extent &last = exts[header->entries - 1];
    auto phys = ((unsigned long) last.phys_hi << 32) | last.phys_lo;
    
    // We can merge this block with the previous entry.
    if (index == last.logical + last.len && value == phys + last.len && last.len < 0x7fff) {
      last.len++;
      // Write back. (If block == 0, then this is a change of metadata, and will be written back later.)
      if (block) {
        auto off = (header->entries - 1) * sizeof(extent) + sizeof(extent_header);
        fs->device->write(fs->offset(block) + off, &last, sizeof(extent), flags);
      }
      return 0;
    }
  }

  // We can't merge, and have to create a new entry.
  if (header->entries == header->max) {
    // Split this node. We need to make sure logical block is always sorted.
    size_t phys = fs->balloc();
    if (phys == -1ul)
      return -ENOSPC;
    extent ext {
      .logical = index,
      .len = 1,
      .phys_hi = (uint16_t) (phys >> 32),
      .phys_lo = (unsigned) phys,
    };
    // We always append at last - no sparse files (TODO).
    return insert_extent(path, header->depth, header->entries, ext);
  }

  // Always append for now. (TODO)
  extent &ext = exts[header->entries];
  ext.logical = index;
  ext.len = 1;
  ext.phys_hi = value >> 32;
  ext.phys_lo = (unsigned) value;
  header->entries++;
  fs->device->mark_dirty(block);
  return 0;
}

int ext_inode::set_pointer(unsigned index, size_t value) {
  if (meta.flags & EXT4_INODE_EXTENTS)
    return set_pointer_ext4(index, value);
  return set_pointer_ext2(index, value);
}

int ext_inode::add_dirent(const string &name, uint32_t inum, uint8_t type) {
  if (name.size() > 255)
    return -ENAMETOOLONG;

  auto fs = static_cast<ext*>(this->fs);
  size_t blksz = fs->blksz;

  for (size_t off = 0; off < meta.sz; off += blksz) {
    size_t b = locate(off);
    // Holes are generally not allowed for directories.
    if (!b)
      fs->on_corrupt();

    char *block = read_block_mutable(b);

    size_t pos = 0;
    while (pos < blksz) {
      // The old entry. We traverse through the entry list.
      auto *de = (direntry *) (block + pos);
      if (de->size < sizeof(direntry) || de->size % 4 != 0 || pos + de->size > blksz)
        fs->on_corrupt();

      size_t len = de->inum ? roundup<4>(sizeof(direntry) + de->namelen) : 0;
      // This is a full entry that occupies the rest of the block.
      // We shrink the previous entry and then create a new one.
      // (Even when inum == 0, the size should also be correct, and the logic is unified.
      if (de->size - len >= roundup<4>(sizeof(direntry) + name.size())) {
        de->size = len;

        auto *ne = (direntry *) (block + pos + len);
        ne->inum = inum;
        ne->namelen = name.size();
        ne->type = type;
        ne->size = blksz - (pos + len);
        memcpy(ne->name, name.c_str(), name.size());

        fs->device->mark_dirty(b);
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

  if (auto ret = set_pointer(meta.sz / blksz, newblk); ret < 0)
    return ret;

  auto sz = (unsigned long) meta.sz + blksz;
  if (fs->size_64) {
    sz += ((unsigned long) meta.acl << 32);
    meta.sz = sz % (1ull << 32);
    meta.acl = sz / (1ull << 32);
  }
  else if (sz >= (1ull << 32))
    return -ENOSPC;
  else
    meta.sz = sz;

  auto block = read_block_mutable(newblk);
  memset(block, 0, blksz);

  auto *entry = (direntry *) block;
  entry->inum = inum;
  entry->namelen = name.size();
  entry->type = type;
  entry->size = blksz;
  memcpy(entry->name, name.c_str(), name.size());
  fs->device->mark_dirty(newblk);
  fs->update_meta(this);

  return 0;
}

ssize_t ext_inode::read(size_t offset, void *buf, size_t len, int flags) {
  auto fs = static_cast<ext*>(this->fs);
  this->flags = flags;

  size_t size = fs->blksz;
  size_t pos = offset;
  size_t end = min((size_t) meta.sz, offset + len);
  ssize_t read = 0;

  meta.atime = now() / 1_s;

  while (pos < end) {
    size_t b = locate(pos);

    if (b == -1ul)
      return read ? read : -ENOSPC;
    size_t chunk = min(size - pos % size, end - pos);

    char *p = (char *) buf + read;

    // Sparse hole.
    if (b == 0)
      memset(p, 0, chunk);
    else if (auto ret = fs->device->read(fs->offset(b) + pos % size, p, chunk, flags); ret < 0)
      return read ? read : ret;

    pos += chunk;
    read += chunk;
  }

  return read;
}

ssize_t ext_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  if (len == 0)
    return 0;
  this->flags = flags;

  bool append = flags & O_APPEND;
  auto fs = static_cast<ext*>(this->fs);
  offset = append ? meta.sz : offset;

  size_t size = fs->blksz;
  size_t pos = offset;
  size_t end = offset + len;
  size_t written = 0;
  size_t entire_size = meta.sz;
  if (fs->size_64)
    entire_size += ((unsigned long) meta.acl << 32);
  if (end > entire_size) {
    if (fs->size_64) {
      meta.sz = end % (1ull << 32);
      meta.acl = end / (1ull << 32);
    }
    else if (end >= 1ull << 32)
      return -ENOSPC;
    else
      meta.sz = end;
  }

  while (pos < end) {
    size_t b = locate(pos);
    if (b == -1ul)
      return written;
    
    if (b == 0) {
      unsigned newblk = fs->balloc();
      if (!newblk)
        break;

      // Attach block to inode.
      if (auto ret = set_pointer(pos / fs->blksz, newblk); ret < 0)
        return ret;

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
  
  size_t time = now() / 1_s;
  meta.mtime = time;
  meta.atime = time;
  fs->update_meta(this);
  return written;
}

ext_inode::ftypeflags ext_inode::fromtype(filetype ty) {
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

int ext_inode::to_dirent_type(filetype ty) {
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

ext_inode::filetype ext_inode::totype(ftypeflags ty) {
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

int ext_inode::create(const string &name, filetype ty, int mode) {
  if (type != Dir)
    return -ENOTDIR;
  meta.atime = meta.mtime = now() / 1_s;
  flags = 0;

  auto fs = static_cast<ext*>(this->fs);
  auto node = fs->get();

  auto pcb = active()->pcb;
  memset(&node->meta, 0, sizeof(node->meta));
  node->meta.sz = 0;
  node->meta.type = fromtype(ty) | mode;
  node->meta.uid = pcb->uid;
  node->meta.gid = pcb->gid;
  node->meta.lnkcnt = (ty == Dir) ? 2 : 1;
  node->meta.ctime = node->meta.mtime = node->meta.atime = now() / 1_s;
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

inode::meta ext_inode::get_meta() {
  return inode::meta(
    meta.atime * 1_s,
    meta.ctime * 1_s,
    meta.mtime * 1_s
  );
}

void ext_inode::set_meta(const inode::meta &meta) {
  this->meta.atime = meta.atime / 1_s;
  this->meta.ctime = meta.ctime / 1_s;
  this->meta.mtime = meta.mtime / 1_s;
}

int ext_inode::unlink(const string &name) {
  if (type != Dir)
    return -ENOTDIR;

  meta.ctime = meta.mtime = now() / 1_s;
  flags = 0;

  auto fs = static_cast<ext*>(this->fs);
  unsigned prevoff = -1;
  for (unsigned pos = 0; pos < meta.sz; prevoff = pos) {
    // Read the directory entry header.
    int block = locate(pos);
    unsigned offset = block * fs->blksz + pos % fs->blksz;
    direntry entry;
    fs->device->read(offset, &entry, sizeof(direntry), flags);
    
    if (entry.size < sizeof(entry))
      fs->on_corrupt();

    pos += entry.size;
    if (entry.inum == 0)
      continue;
    
    unique_ptr<char[]> p(new char[entry.namelen + 1]);
    fs->device->read(offset + sizeof(entry), p.get(), entry.namelen, flags);
    p[entry.namelen] = '\0';
    if (name != p.get())
      continue;

    // The node itself must also be unlinked.
    // The file system caches all inodes, so that the same inode on disk will result in the same inode in memory.
    auto inode = fs->read_from_inum(entry.inum);
    if (inode->type == Dir)
      return -EISDIR;
    
    // Now we unlink it: merge the length to previous header.
    bool first = pos % fs->blksz == 0;
    if (first) {
      // When there is no previous header, simply set inum to zero.
      entry.inum = 0;
      fs->device->write(offset, &entry, sizeof(direntry), flags);
    } else {
      int prevblk = locate(prevoff);
      prevoff = prevblk * fs->blksz + prevoff % fs->blksz;

      direntry prev;
      fs->device->read(prevoff, &prev, sizeof(direntry), 0);
      prev.size += entry.size;
      fs->device->write(prevoff, &prev, sizeof(direntry), 0);
    }

    // Forget it in dcache. This isn't done here; see (the unique) caller, handler of `unlinkat`.
    inode->meta.lnkcnt--;
    
    fs->update_meta(inode);
    inode->unlinked();
    fs->update_meta(this);
    return 0;
  }
  return -ENOENT;
}

inode *ext_inode::lookup(const string &name) {
  if (type != Dir)
    return nullptr;
  meta.atime = now() / 1_s;
  flags = 0;

  auto fs = static_cast<ext*>(this->fs);

  // Do batch reading: read a block each time, rather than locate()'ing every single entry.
  for (unsigned pos = 0; pos < meta.sz; ) {
    unsigned off = pos % fs->blksz;
    
    size_t b = locate(pos);
    if (b == 0)
      return nullptr;
    const char *data = read_block(b);

    // Iterate through all entries that live inside this 4KB block.
    while (off < fs->blksz && pos < meta.sz) {
      auto entry = (const direntry *) (data + off);
      
      if (entry->size < sizeof(direntry) || off + entry->size > fs->blksz)
        fs->on_corrupt(); // Safety check

      if (entry->inum != 0) {
        if (name.size() == entry->namelen && 
          strncmp(name.c_str(), (char *) (entry + 1), entry->namelen) == 0) {
          return fs->read_from_inum(entry->inum);
        }
      }
      
      pos += entry->size;
      off += entry->size;
    }
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

vector<inode::item> ext_inode::list() {
  if (type != Dir)
    return {};
  meta.atime = now() / 1_s;
  flags = 0;

  auto fs = static_cast<ext*>(this->fs);
  vector<item> result;
  for (unsigned pos = 0; pos < meta.sz;) {
    // Read the directory entry header.
    int block = locate(pos);
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

optional<string> ext_inode::readlink() {
  if (type != filetype::Link)
    return nullopt;
  meta.atime = now() / 1_s;
  
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

int ext_inode::erase_ext2(unsigned block, int level) {
  if (block == 0)
    return 0;

  auto fs = (ext *) this->fs;
  if (level == 0) {
    fs->free_block(block);
    return 1;
  }

  auto page = (unsigned *) read_block(block);
  if (!page)
    return -EIO;

  for (int i = fs->blksz / sizeof(unsigned); i >= 0; i--) {
    if (!page[i])
      continue;
    if (auto ret = erase_ext2(page[i], level - 1); ret < 0)
      return ret;
  }

  fs->free_block(block);
  return 0;
}

int ext_inode::erase_ext2(unsigned block, unsigned base, unsigned first, unsigned last, int level) {
  if (block == 0)
    return 0;

  auto fs = (ext *)this->fs;
  const size_t N = fs->blksz / sizeof(unsigned);

  size_t span = 1;
  for (int i = 0; i < level; i++)
    span *= N;

  size_t end = base + span - 1;

  // No overlap. Return.
  if (last < base || first > end)
    return 0;

  // Fully covered. Free entire subtree.
  if (first <= base && end <= last) {
    if (auto ret = erase_ext2(block, level); ret < 0)
      return ret;
    return span;
  }

  // Partial overlap. For level-0 nodes, there shouldn't be "partial".
  assert(level != 0);

  auto page = (unsigned *) read_block(block);
  if (!page)
    return -EIO;

  int freed = 0;
  bool all_zero = false;

  for (size_t i = 0; i < N; i++) {
    if (!page[i])
      continue;

    int ret = erase_ext2(page[i], end + i * (span / N), first, last, level - 1);
    if (ret < 0)
      return ret;

    freed += ret;
    // The child is fully freed.
    if (ret == int(span / N))
      page[i] = 0, all_zero = true;
  }

  if (all_zero)
    fs->free_block(block);

  return freed;
}

int ext_inode::truncate_ext2(size_t len) {
  auto fs = (ext *) this->fs;

  // Shrink case.
  if (len < meta.sz) {
    size_t begin = (len + fs->blksz - 1) / fs->blksz;
    size_t end = (meta.sz + fs->blksz - 1) / fs->blksz - 1;

    // Remove all blocks in [begin, end].
    const int N = fs->blksz / sizeof(unsigned);
    erase_ext2(meta.indirect3, 12 + N + N * N, begin, end, 0);
    erase_ext2(meta.indirect2, 12 + N, begin, end, 0);
    erase_ext2(meta.indirect1, 12, begin, end, 0);
    for (int i = 12; i >= 0; i--)
      erase_ext2(meta.directptr[i], i, begin, end, 0);

    meta.sz = len;
    fs->update_meta(this);
    return 0;
  }

  // Grow case. `write` and `read` will handle holes.
  meta.sz = len;
  return 0;
}

int ext_inode::truncate_ext4(size_t) {
  assert(false && "ext4: no truncate yet"); // TODO
}

int ext_inode::truncate(size_t len) {
  if (type == Dir)
    return -EISDIR;
  meta.ctime = meta.mtime = now() / 1_s;

  if (meta.flags & EXT4_INODE_EXTENTS)
    return truncate_ext4(len);
  return truncate_ext2(len);
}

// Currently we're assuming ext2 header starts at sector 2. This isn't always the case; read sectors 0 & 1 to know.
ext::ext(block_inode *device): device(device) {
  constexpr auto sbsz = sizeof(struct superblock);
  unique_ptr<char[]> block(new char[sbsz]);
  device->read(1024, block.get(), sbsz, 0);
  memcpy(&superblock, block.get(), sbsz);
  // Leave ext2 in an uninitialized state. In this state, this->root is nullptr,
  // so it's easy to detect an error.
  if (superblock.magic != 0xef53)
    return;

  if (superblock.ver_major < 1) {
    superblock.inode_size = 128;
    superblock.first_non_reserved = 11;
  }
  
  // Look at features. See https://wiki.osdev.org/Ext4
  extent = superblock.required_features & 0x40;
  do_crc = superblock.readonly_features & 0x400;
  fs_64 = superblock.required_features & 0x80;
  size_64 = superblock.readonly_features & 0x2;
  if (superblock.required_features & ~0x2c6) {
    printk("mount: cannot understand required features %x\n", superblock.required_features);
    return;
  }
  if (superblock.readonly_features & ~0x46b) {
    printk("mount: cannot understand readonly features %x\n", superblock.readonly_features);
    return;
  }
  if (fs_64)
    gdsz = superblock.gd_size;

  blksz = 1 << (10 + superblock.block_size);
  size_t total_blocks = read_64(superblock.total_blocks, superblock.total_blocks_hi);
  auto group_count = (total_blocks + superblock.block_per_group - 1) / superblock.block_per_group;

  int gdt_start = (blksz == 1024) ? 2 : 1;
  gdt.resize(group_count);

  size_t len = group_count * gdsz;
  char *buf = new char[len];
  device->read(gdt_start * blksz, buf, len, 0);
  // Note this is not necessarily contiguous.
  for (unsigned i = 0; i < group_count; i++)
    memcpy(&gdt[i], buf + gdsz * i, len);
  delete[] buf;

  // Root is always at inode 2.
  root = new dentry("", read_from_inum(2), nullptr);
  printk("ext2 version = %d.%d\n", superblock.ver_major, superblock.ver_minor);
  printk("enabled features: %x %x %x\n", superblock.required_features, superblock.readonly_features, superblock.optional_features);
}

size_t ext::offset(size_t id) {
  return (id - superblock.start_block_num) * blksz;
}

void ext::update_superblock() {
  device->write(1024, &superblock, fs_64 ? sizeof(struct superblock) : offsetof(struct superblock, __64bit_start), 0);
}

void ext::update_block_group(int id) {
  int gdt_start = (blksz == 1024) ? 2 : 1;
  device->write(gdt_start * blksz + id * gdsz, &gdt[id], gdsz, 0);
}

ext_inode *ext::search_inode(int groupid, block_group &gd) {
  size_t cnt = read_32(gd.free_inodes_count, gd.free_inodes_count_hi);
  size_t unused = read_32(gd.never_used_inodes, gd.never_used_inodes_hi);
  if (!cnt)
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
      
      size_t inum = groupid * superblock.inodes_per_group + i * 8 + j + 1;
      bitmap[i] |= (1 << j);
      
      write_32(--cnt, gd.free_inodes_count, gd.free_inodes_count_hi);
      if (unused > 0)
        unused--;
      write_32(unused, gd.never_used_inodes, gd.never_used_inodes_hi);
      // Note this does not have a "high" field even in 64-bit mode.
      superblock.free_inodes--;
      
      // Write back changes.
      device->write(offset(gd.inode_bitmap), bitmap.get(), blksz, 0);
      update_block_group(groupid);
      update_superblock();

      return new ext_inode(this, inum);
    }
  }
  return nullptr;
}

size_t ext::search_block(int groupid, block_group &gd) {
  size_t cnt = read_32(gd.free_blocks_count, gd.free_blocks_count_hi);
  size_t free = read_64(superblock.free_blocks, superblock.free_blocks_hi);
  if (!cnt)
    return -1;

  unique_ptr<char[]> bitmap(new char[blksz]);
  size_t block = read_64(gd.block_bitmap, gd.block_bitmap_hi);
  device->read(offset(block), bitmap.get(), blksz, 0);

  // Search the Bitmap for the first free bit, 0.
  for (unsigned i = 0; i < blksz; i++) {
    unsigned char byte = bitmap[i];
    
    // 0 is free and 1 is occupied. Skip the fully occupied byte.
    if (byte == 0xFF)
      continue;

    for (int j = 0; j < 8; j++) {
      if (byte & (1 << j))
        continue;
      
      size_t inum = size_t(groupid) * superblock.inodes_per_group + i * 8 + j + 1;
      bitmap[i] |= (1 << j);
      
      write_32(--cnt, gd.free_blocks_count, gd.free_blocks_count_hi);
      write_64(--free, superblock.free_blocks, superblock.free_blocks_hi);
      
      // Write back changes.
      device->write(offset(block), bitmap.get(), blksz, 0);
      update_block_group(groupid);
      update_superblock();

      return inum;
    }
  }
  return -1;
}

ext_inode *ext::get() {
  for (unsigned i = 0; i < gdt.size(); i++) {
    if (auto ret = search_inode(i, gdt[i]))
      return ret;
  }
  return nullptr;
}

size_t ext::balloc() {
  for (unsigned i = 0; i < gdt.size(); i++) {
    if (auto ret = search_block(i, gdt[i]))
      return ret;
  }
  return -1;
}

void ext::free_blocks(ext_inode *node, size_t block, int level) {
  if (!block)
    return;

  auto data = device->get_page(offset(block) / PAGE_SIZE);
  if (!data)
    return;

  auto p = (unsigned *) data;
  free_block(block);
  if (level == 1) {
    for (unsigned i = 0; i < blksz / sizeof(int); i++)
      free_block(p[i]);
    device->mark_dirty(offset(block) / PAGE_SIZE);
    return;
  }

  for (unsigned i = 0; i < blksz / sizeof(int); i++) {
    size_t pos = p[i];
    free_blocks(node, pos, level - 1);
  }
}

void ext::erase(inode *n) {
  auto node = cast<ext_inode>(n);
  if (nodecache.count(node->_inum))
    nodecache.erase(node->_inum);

  // Free all blocks of this inode.
  // free_block(0) will be a no-op.
  if (!extent) {
    for (auto x : node->meta.directptr)
      free_block(x);

    free_blocks(node, node->meta.indirect1, 1);
    free_blocks(node, node->meta.indirect2, 2);
    free_blocks(node, node->meta.indirect3, 3);
  } else {
    panic("no extent yet"); // TODO
  }

  // Free the inode.
  auto inum = node->_inum;

  // Zero the inode.
  memset(&node->meta, 0, sizeof(struct ext_inode::meta));
  update_meta(node);

  free_inode(inum);
}

// These are a pair of functions that read/write metadata.

void ext::update_meta(ext_inode *node) {
  unsigned inum = node->_inum;
  unsigned group = (inum - 1) / superblock.inodes_per_group;
  unsigned index = (inum - 1) % superblock.inodes_per_group;

  const block_group &gd = gdt[group];

  size_t table = read_64(gd.inode_table, gd.inode_table_hi);
  size_t offset = table * blksz + index * superblock.inode_size;
  device->write(offset, &node->meta, sizeof(node->meta), 0);
  device->write(offset + sizeof(node->meta), zeroes, superblock.inode_size - sizeof(node->meta), 0);
}

void ext::free_inode(size_t inum) {
  if (inum == 0 || inum >= superblock.total_inodes)
    return;

  unsigned group = (inum - 1) / superblock.inodes_per_group;
  block_group &gd = gdt[group];

  size_t bitmap = read_64(gd.inode_bitmap, gd.inode_bitmap_hi);
  size_t offset = bitmap * blksz + (inum - 1) / 8;
  int bit = (inum - 1) % 8;
  unsigned char value;
  
  device->read(offset, &value, 1, 0);
  if (!(value & (1 << bit)))
    panic("ext2: free_inode: double free");
  value &= ~(1 << bit);
  device->write(offset, &value, 1, 0);

  // Note this doesn't have a 64-bit version.
  superblock.free_inodes++;
  unsigned free = read_32(gd.free_inodes_count, gd.free_inodes_count_hi);
  write_32(free + 1, gd.free_inodes_count, gd.free_inodes_count_hi);
}

void ext::free_block(size_t block) {
  if (block == 0 || block >= superblock.total_blocks)
    return;

  unsigned group = (block - 1) / superblock.block_per_group;
  block_group &gd = gdt[group];

  size_t bitmap = read_64(gd.block_bitmap, gd.block_bitmap_hi);
  size_t offset = bitmap * blksz + (block - 1) / 8;
  int bit = (block - 1) % 8;
  unsigned char value;

  device->read(offset, &value, 1, 0);
  if (!(value & (1 << bit)))
    panic("ext2: free_inode: double free");
  value &= ~(1 << bit);
  device->write(offset, &value, 1, 0);
  
  unsigned free = read_32(gd.free_blocks_count, gd.free_blocks_count_hi);
  write_32(free + 1, gd.free_blocks_count, gd.free_blocks_count_hi);

  size_t free_ = read_64(superblock.free_blocks, superblock.free_blocks_hi);
  write_64(free_ + 1, superblock.free_blocks, superblock.free_blocks_hi);
}

ext_inode *ext::read_from_inum(size_t inum) {
  if (nodecache.count(inum))
    return nodecache[inum];

  auto group = (inum - 1) / superblock.inodes_per_group;
  auto index = (inum - 1) % superblock.inodes_per_group;

  const block_group &gd = gdt[group];
  // Compute byte offset.
  size_t table = read_64(gd.inode_table, gd.inode_table_hi);
  auto offset = table * blksz + index * superblock.inode_size;

  auto meta = (struct ext_inode::meta*) vmalloc(superblock.inode_size);
  device->read(offset, meta, superblock.inode_size, 0);
  auto inode = new ext_inode(this, *meta, inum);
  vfree(meta);
  return nodecache[inum] = inode;
}

void ext::sync() {
  device->flush();
}

size_t ext::read_64(uint32_t lo, uint32_t hi) {
  size_t result = lo;
  if (fs_64)
    result += ((unsigned long) hi << 32);
  return result;
}

int ext::write_64(size_t v, uint32_t &lo, uint32_t &hi) {
  if (v >= (1ull << 32) && !fs_64)
    return -ENOSPC;
  if (fs_64) {
    lo = v % (1ull << 32);
    hi = v / (1ull << 32);
  } else
    lo = v;
  return 0;
}

size_t ext::read_32(uint16_t lo, uint16_t hi) {
  size_t result = lo;
  if (fs_64)
    result += ((unsigned long) hi << 32);
  
  return result;
}

int ext::write_32(size_t v, uint16_t &lo, uint16_t &hi) {
  if (v >= (1ull << 16) && !fs_64)
    return -ENOSPC;
  if (fs_64) {
    lo = v % (1ull << 16);
    hi = v / (1ull << 16);
  } else
    lo = v;
  return 0;
}

expected<fs*> ext_creator(const char *src) {
  auto tcb = active();
  auto pcb = tcb->pcb;
  
  int fd = pcb->open_file(src, 0);
  if (fd < 0)
    return -EBADF;
  inode *node = pcb->ftbl->at(fd)->node();
  auto blk = dyn_cast<block_inode>(node);
  if (node->type != inode::BlockDevice || !blk)
    return -ENOTBLK;
  auto ext = new class ext(blk);
  if (!ext->root)
    return -EINVAL;
  return ext;
}

}
