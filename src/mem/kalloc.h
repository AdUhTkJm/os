#ifndef KALLOC_H
#define KALLOC_H

#include "../utils/helper.h"

namespace os {

/* Gives a free 4KB physical frame. Note this expects a physical address. */
void *pframe();
/* Frees a 4KB physical frame. Note this expects a physical address. */
void pfree(void *p);

/* Allocates a `len`-byte consecutive virtual memory. */
void *vmalloc(size_t len);
/* Free a chunk of memory allocated by `vmalloc`. */
void vfree(void *p);

// Initialize the bitmap allocator.
void init_pm_allocator();

}

#endif
