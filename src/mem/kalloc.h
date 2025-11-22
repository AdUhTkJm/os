#ifndef KALLOC_H
#define KALLOC_H

#include "../utils/helper.h"
#include "ptable.h"

namespace os {

/* Gives a free 4KB physical frame. Note this expects a physical address. */
pa_t pframe();
/* Frees a 4KB physical frame. Note this expects a physical address. */
void pfree(pa_t p);

/* Free a chunk of memory allocated by `vmalloc`. */
void vfree(void *p);

// Initialize the bitmap allocator.
void init_bitmap_kalloc();
void init_freelist_kalloc();

template<size_t Align = sizeof(size_t)> requires (Align >= sizeof(size_t) && __builtin_popcount(Align) == 1)
void *vmalloc(size_t len) {
  // This will return a 8-bit aligned address.
  void *vmalloc_impl(size_t);
  size_t *v = (size_t *) vmalloc_impl(len + Align);
  size_t *ptr = roundup<Align>(v);
  for (auto p = v; p != ptr; p++)
    *p = 0;
  return ptr;
}

template<size_t Align> requires (Align < sizeof(size_t))
void *vmalloc(size_t len) {
  return vmalloc<sizeof(size_t)>(len);
}

}

#endif
