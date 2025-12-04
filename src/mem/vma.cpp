#include "vma.h"
#include "../proc/schedule.h"

namespace os {

void vma_map_single(void *va, pte_t *root) {
  EnableAccessToUserMemory enabler;
  auto pcb = scheduler.active;

  va_t addr = (va_t) va;
  auto va_page = rounddown<4_kb>(va);
  int origflags = pte_flags(va_page);

  size_t i = 0;
  for (; i < pcb->vma.size(); i++) {
    const auto &vma = pcb->vma[i];
    if (addr <= vma.end && addr >= vma.begin)
      break;
  }
  if (i == pcb->vma.size()) {
    printk("Unmapped address %p. Terminate the process.\n", va);
    os::terminate(pcb, -127);
    return;
  }
  auto pa = os::pframe();
  const auto &vma = pcb->vma[i];
  int flags = PTE_V | PTE_U;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;

  if (origflags != -1 && (origflags & PTE_COW)) {
    // This is a copy-on-write segment. We copy the original contents.
    memcpy((void *) as_va(pa), va_page, PAGE_SIZE);
    // Remap the memory and let it point to the new pa.
    os::pmap(pa, va_page, MAP_4KB, flags | PTE_W, root);
    return;
  }

  // Temporarily map with write permission, if we need to copy into it.
  os::pmap(pa, va_page, MAP_4KB, vma.backup ? flags | PTE_W : flags, root);

  // Copy the contents if it exists.
  if (!vma.backup)
    return;

  // Take back the write permission on exit.
  const auto &finisher = [&]() {
    if (!(flags & PTE_W))
      os::pmap(pa, va_page, MAP_4KB, flags & ~PTE_W, root);
  };
  struct takeback {
    decltype(finisher) f;
    takeback(decltype(finisher) f): f(f) {}
    ~takeback() { f(); }
  } _takeback(finisher);
  
  // `begin` and this address are in the same page.
  // We read from beginning.
  if (vma.begin / PAGE_SIZE == addr / PAGE_SIZE) {
    SeekGuard guard(vma.backup, vma.offset);
    auto end = min(rounddown<4_kb>(vma.begin + PAGE_SIZE), vma.end);
    auto read = vma.backup->read((void *) vma.begin, end - vma.begin);
    memset((char*) va_page + read, 0, PAGE_SIZE - read);
    return;
  }

  // This is in the middle. We read the entire page.
  SeekGuard guard(vma.backup, vma.offset + ((va_t) va_page - vma.begin));
  auto read = vma.backup->read(va_page, PAGE_SIZE);
  memset((char*) va_page + read, 0, PAGE_SIZE - read);
  printk("va = %p (data = %p)\n", va, *(size_t *) va);
}

void vma_map_current(void *va) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return;

  return vma_map_single(va, pt_root());
}

void vma_map_current(void *va, pte_t *root) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return;

  return vma_map_single(va, root);
}

void vma_map_current(void *from, void *to, bool write) {
  char *p = (char *) from, *q = (char *) to;
  for (; p < q; p += PAGE_SIZE) {
    int flags = pte_flags(p);
    if (flags == -1 || (flags & PTE_COW && !(flags & PTE_W) && write))
      vma_map_single(p, pt_root());
  }
}

}

