#ifndef KALLOC_H
#define KALLOC_H

#include "ptable.h"
#include "../instr/leak.h"
#include "../utils/helper.h"

namespace os {

namespace zero {

inline pa_t head;
inline unsigned len;
inline spinlock lock;

}

struct pframe_meta {
  unsigned char refcnt;
  // Type in slabs.
  unsigned char type;
  // Type in buddy allocator.
  unsigned char order;
};

struct vframe_meta {
  unsigned char refcnt;
  unsigned char order;
};

// Reserved kernel virtual memory size.
// We will reserve some final maps for VDSO.
constexpr size_t VM_SIZE = 4_gb - PAGE_SIZE;
constexpr va_t VM_BASE = 0xffff'ffff'0000'0000ul;

// The entire physical memory space we're able to manage.
constexpr va_t MAX_PA_SIZE = 1_gb;

// This amount of 4KB frames from __kernel_base will be managed by
// the free-list allocator, mainly for bootstrapping.
// All other regions will be managed by the bitmap allocator.
constexpr va_t FREE_LIST_SIZE = 0x1000;

// Gives a free 4KB physical frame.
pa_t pframe();
// Gives a zeroed free 4KB physical frame.
pa_t pframe_zeroed();
// Frees a 4KB physical frame.
// If the reference count is greater than 1, does not free,
// but decreases the reference count.
void pfree(pa_t p);
// Increase the page's reference count by 1.
void pincref(pa_t p);
// Decreases the page's reference count by 1.
void drop(pa_t p);

// Gives a consecutive chunk of physical memory.
pa_t pmalloc(int pagecnt);

// Gives the number of free pages in the system.
size_t pavail();
// Gives the number of total pages in the system.
size_t ptotal();
// Total amount of (virtually) shared memory.
inline size_t pshared;

// Free a chunk of memory allocated by `vmalloc`.
void vfree(void *p);

// Initialize the buddy/slab allocators.
void init_kalloc();

// The zeroed free list.
void make_zeroes();

template<size_t Align = sizeof(size_t)> requires (Align >= sizeof(size_t) && __builtin_popcount(Align) == 1)
void *vmalloc(size_t len, bool permanent = false) {
  // This will return a 8-bit aligned address.
  void *vmalloc_impl(size_t);
  size_t *v = (size_t *) vmalloc_impl(len + Align);
  size_t *ptr = roundup<Align>(v);
#ifdef FUNC_INSTRUMENT
  if (!permanent)
    leak::record_alloc(ptr, len);
#else
  (void) permanent;
#endif
  return ptr;
}

template<size_t Align> requires (Align < sizeof(size_t))
void *vmalloc(size_t len) {
  return vmalloc<sizeof(size_t)>(len);
}

pframe_meta *inspect_meta();
size_t off(pa_t pa);
int refcnt(pa_t pa);

}

#endif
