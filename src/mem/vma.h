#ifndef VMA_H
#define VMA_H

#include "ptable.h"
#include "../fs/vfs.h"
#include "../utils/log.h"
#include "../utils/stl/intervaltree.h"

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

namespace os {

template<class T>
struct less {
  bool operator()(const T& a, const T& b) const {
    return a < b;
  }
};

}

namespace os::vma {

struct vma_t {
  uintptr_t begin, end;
  int prot, flags;
  file *backup;
  size_t offset, maxread;

  vma_t(): backup(nullptr) {}
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags);
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags, file *backup, size_t offset, size_t maxread);
  vma_t(const vma_t &other);
  vma_t(vma_t &&other);
  ~vma_t();

  vma_t &operator=(const vma_t &other);

  bool mergeable(const vma_t &other) const;
};

// Map according to the current process's VMA.
// Terminates the process when the pointer is not in any VMA.
[[nodiscard]] bool map_single(void *va, pte_t *root);

[[nodiscard]] bool map_current(void *va);
[[nodiscard]] bool map_current(void *va, pte_t *pte);

// Map a range. Only maps the addresses that are currently unmapped.
// If `write` is set to true, also maps COW pages in the range.
[[nodiscard]] bool map_current(void *from, void *to, bool write = false);

// Initialize VMA.
void init();

struct addrspace : shared {
  using map = interval_btree<vma_t, 4>;
  using node = map::node;

  map vmas;
  // We actually have a map for [heap_begin, heap_end), but the current program break is `brkp`.
  va_t heap_begin, heap_end, brkp;
  va_t mmap_begin = 0x6000'0000;
  mutable vma_t *cache;

#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT_VMA)
  void ondrop() override;
  void onref() override;
#endif

  addrspace() = default;
  ~addrspace() { vmas.clear(); }

  void split(va_t addr);
  // Unlike btree::find, this finds the VMA *containing* the address, rather than starting at the address.
  vma_t *find(va_t addr) const;
  va_t brk(va_t addr);
  va_t find_mmap(unsigned long len, va_t hint) const;

  void insert(const vma_t &vma);
  void erase(va_t begin) { vmas.erase(begin); }
  void clear() { vmas.clear(); }

  auto find_overlap(va_t begin, va_t end) const {
    return vmas.find_overlap(begin, end);
  }
};

}

#endif
