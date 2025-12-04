#ifndef KALLOC_H
#define KALLOC_H

#include "ptable.h"
#include "../instr/leak.h"
#include "../utils/helper.h"

namespace os {

// Gives a free 4KB physical frame.
pa_t pframe();
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
// Gives a consecutive chunk in the lower 4GB, for


/* Free a chunk of memory allocated by `vmalloc`. */
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

}

#endif
