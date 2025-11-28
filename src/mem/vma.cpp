#include "vma.h"
#include "../proc/schedule.h"

namespace os {

void vma_map_single(void *va, pte_t *root, pcb_t *pcb) {
  EnableAccessToUserMemory enabler;

  va_t addr = (va_t) va;
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
  int flags = PTE_V | PTE_U | PTE_RWX;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;
  auto va_page = rounddown<4_kb>(va);
  os::pmap(pa, va_page, MAP_4KB, flags, root);

  // Copy the contents if it exists.
  if (!vma.backup)
    return;
  
  // `begin` and this address are in the same page.
  // We read from beginning.
  if (vma.begin / PAGE_SIZE == addr / PAGE_SIZE) {
    SeekGuard guard(vma.backup, vma.offset);
    auto end = min(rounddown<4_kb>(vma.begin + PAGE_SIZE), vma.end);
    vma.backup->read((void *) vma.begin, end - vma.begin);
    return;
  }

  // `end` and this address are in the same page.
  // We shouldn't read past the end.
  if (vma.end / PAGE_SIZE == addr / PAGE_SIZE) {
    SeekGuard guard(vma.backup, vma.offset + ((va_t) va_page - vma.begin));
    vma.backup->read(va_page, vma.end - (va_t) va_page);
    return;
  }

  // This is in the middle. We read the entire page.
  SeekGuard guard(vma.backup, vma.offset + ((va_t) va_page - vma.begin));
  vma.backup->read(va_page, PAGE_SIZE);
}

void vma_map_current(void *va, pte_t *root) {
  return vma_map_single(va, root, scheduler.active);
}

void vma_map_current(void *from, void *to) {
  char *p = (char *) from, *q = (char *) to;
  for (; p < q; p += PAGE_SIZE) {
    if (pte_flags(p) == -1)
      vma_map_current(p);
  }
}

void vma_map(void *from, void *to, pcb_t *pcb) {
  char *p = (char *) from, *q = (char *) to;
  pte_t *root = (pte_t *) as_va(pcb->pt_root);
  for (; p < q; p += PAGE_SIZE) {
    if (pte_flags(p, root) == -1)
      vma_map_single(p, root, pcb);
  }
}

}

