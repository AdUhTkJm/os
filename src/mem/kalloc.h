#ifndef KALLOC_H
#define KALLOC_H

#include "../utils/helper.h"
#include "ptable.h"

namespace os {

/* Gives a free 4KB physical frame. Note this expects a physical address. */
pa_t pframe();
/* Frees a 4KB physical frame. Note this expects a physical address. */
void pfree(pa_t p);

/* Allocates a `len`-byte consecutive virtual memory. */
void *vmalloc(size_t len);
/* Free a chunk of memory allocated by `vmalloc`. */
void vfree(void *p);

// Initialize the bitmap allocator.
void init_bitmap_kalloc();
void init_freelist_kalloc();

}

#endif
