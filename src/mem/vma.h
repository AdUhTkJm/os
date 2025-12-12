#ifndef VMA_H
#define VMA_H

#include "../utils/helper.h"
#include "../fs/vfs.h"
#include "ptable.h"

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Kernel internal usage, used for brk().
#define VMA_IS_HEAP    0x40
#define VMA_IS_STACK   0x80
#define VMA_IS_PT_LOAD 0x100

namespace os::vma {

struct vma_t {
  uintptr_t begin, end;
  int prot, flags;
  file *backup;
  size_t offset, maxread;

  vma_t() = default;
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags);
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags, file *backup, size_t offset, size_t maxread);
  vma_t(const vma_t &other);
  ~vma_t();

  vma_t &operator=(const vma_t &other);

  bool mergeable(const vma_t &other) const;
};

// Map according to the current process's VMA.
// Terminates the process when the pointer is not in any VMA.
void map_current(void *va);
void map_current(void *va, pte_t *pte);

// Map a range. Only maps the addresses that are currently unmapped.
// If `write` is set to true, also maps COW pages in the range.
void map_current(void *from, void *to, bool write = false);

struct vmas {
  vector<vma_t> vmas;

  void split_at(size_t i, va_t addr);
  result merge_at(size_t i);
  
  vma_t &operator[](size_t index) { return vmas[index]; }
  const vma_t &operator[](size_t index) const { return vmas[index]; }

  result push(const vma_t &vma);
  bool has(va_t addr) const;

  // Finds the insertion place of `addr`, i.e. the first vma that is
  // greater than `addr`.
  // If `addr` is already contained, return that index.
  size_t find(va_t addr) const;
  
  vma_t &at(va_t addr) { return vmas[find(addr)]; }
  const vma_t &at(va_t addr) const { return vmas[find(addr)]; }
  void clear() { vmas.clear(); }

  vma_t *begin() { return vmas.begin(); }
  vma_t *end() { return vmas.end(); }
  size_t size() const { return vmas.size(); }

  void resize(size_t sz) { vmas.resize(sz); }
};

}

#endif
