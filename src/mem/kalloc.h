#ifndef KALLOC_H
#define KALLOC_H

#include "../utils/helper.h"

/* Gives a free 4KB physical frame. Note this expects a physical address. */
C void *pframe();
/* Frees a 4KB physical frame. Note this expects a physical address. */
C void pfree(void *p);

/* Allocates a `len`-byte consecutive virtual memory. */
C void *vmalloc(size_t len);
/* Free a chunk of memory allocated by `vmalloc`. */
C void vfree(void *p);

#endif
