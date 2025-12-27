#ifndef KALLOC_H
#define KALLOC_H

#include "ptable.h"
#include "../instr/leak.h"
#include "../utils/helper.h"

namespace os {

struct pframe_meta {
  // 0 for normal memory; non-zero value `i` for slabs[i].
  unsigned char type;
  unsigned char refcnt;
};

// Reserved kernel virtual memory size.
constexpr size_t VM_SIZE = 1_gb;
constexpr va_t VM_BASE = 0xffff'ffff'c000'0000ul;

// The entire physical memory space we're able to manage. QEMU only has 128MB anyway.
// When we enable DEBUG_MEMORY, the meta becomes incredibly large.
#if defined(DEBUG_MEMORY) && defined(FUNC_INSTRUMENT)
constexpr va_t MAX_PA_SIZE = 128_mb;
#else
constexpr va_t MAX_PA_SIZE = 2_gb;
#endif

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

// Initialize the bitmap allocator.
void init_bitmap_kalloc();
void init_freelist_kalloc();

template<size_t Align = sizeof(size_t)> requires (Align >= sizeof(size_t) && __builtin_popcount(Align) == 1)
void *vmalloc(size_t len, bool permanent = false) {
  // This will return a 8-bit aligned address.
  void *vmalloc_impl(size_t);
  size_t *v = (size_t *) vmalloc_impl(len + Align);
  size_t *ptr = roundup<Align>(v);
  for (auto p = v; p != ptr; p++)
    *p = 0;
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

#ifndef NDEBUG
pframe_meta *inspect_meta();
size_t off(pa_t pa);
#endif

}

#endif
