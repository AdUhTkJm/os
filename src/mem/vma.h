#ifndef VMA_H
#define VMA_H

#include "../utils/helper.h"
#include "../fs/vfs.h"

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

namespace os {

struct vma_t {
  uintptr_t begin, end;
  int prot, flags;
  file *backup;
  size_t offset;
};

// Map according to the current process's VMA.
// Terminates the process when the pointer is not in any VMA.
void vma_map_current(void *va);

// Map a range. Only maps the addresses that are currently unmapped.
void vma_map_current(void *from, void *to);

}

#endif
