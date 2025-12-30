#include "vma.h"
#include "../proc/schedule.h"

namespace os::vma {

[[nodiscard]] bool map_single(void *va, pte_t *root) {
  EnableAccessToUserMemory enabler;
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto v = now();
  va_t addr = (va_t) va;
  auto va_page = rounddown<4_kb>(va);
  pte_t pte = pte_of((va_t) va_page, root);

  auto vmap = pcb->vma->find(addr);
  if (!vmap) {
    // Examine scause to print a better debug message. This is mainly for RISC-V.
#ifdef RV
    int scause; CSRR(scause, scause);
    auto type = scause == 12 ? "execute" : scause == 13 ? "load" : scause == 15 ? "store" : nullptr;
    if (type) {
      va_t sepc = ((trapframe *) tcb->ksp)->sepc;
      printk("Unmapped address %p on %s, requested from instruction %p of process %d (thread %d).\n",
        va, type, sepc, pcb->pid, tcb->tid);
    } else
#endif
      printk("Unmapped address %p.\n", va);
    return false;
  }
  const auto &vma = *vmap;

  pa_t pa = (vma.flags & MAP_SHARED)
    ? (pa_t) (*vma.backup->node()->cache)[(addr - vma.begin + vma.offset) / PAGE_SIZE]->data - KERNEL_OFFSET
    : os::pframe_zeroed();
  
  int flags = PTE_V | PTE_U;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;
  if (vma.flags & MAP_SHARED) {
    flags |= PTE_SHARED;
    pincref(pa);
  }

  if ((pte & PTE_V) && (pte & PTE_COW)) {
    assert(!(pte & PTE_SHARED));
    // This is a copy-on-write segment. We copy the original contents.
    memcpy((void *) as_va(pa), va_page, PAGE_SIZE);
    // The original pa must be freed.
    pfree(PTE_TO_PA(pte));
    // Remap the memory and let it point to the new pa.
    os::pmap(pa, va_page, MAP_4KB, flags | PTE_W, root);
    return true;
  }

  if (!vma.backup) {
    tcb->ruse.ru_minflt++;
    os::pmap(pa, va_page, MAP_4KB, flags, root);
    // This is uniprocessor and no sleep can happen, so no worry about the race condition below.
    return true;
  }

  // Copy the contents if it exists.
  // We must map after reading, in case of CLONE_VM is specified.
  // Consider this case:
  //   Thread A accesses `va`
  //   Thread A page faults
  //   Thread A maps a page and calls read(), suspends
  //   Thread B swapped in
  //   Thread B writes `va` <- BOOM (1): sees zeroes!
  //   Thread A remaps `va` <- BOOM (2): lost data!
  // We must map the page after the read.

  tcb->ruse.ru_majflt++;
  
  if (vma.begin / PAGE_SIZE == addr / PAGE_SIZE) {
    // `begin` and this address are in the same page.
    // We read from beginning.
    SeekGuard guard(vma.backup, vma.offset);
    auto off = vma.begin % PAGE_SIZE;
    auto read = min(PAGE_SIZE - off, vma.maxread);
    vma.backup->read((void *) as_va(vma.begin - (va_t) va_page + pa), read);
  } else {
    // This is in the middle. We read the entire page.
    va_t off = (va_t) va_page - vma.begin;
    // Note that these are unsigned, so a direct subtraction and then max(..., 0) won't work.
    ssize_t read = off < vma.maxread ? min((size_t) PAGE_SIZE, vma.maxread - off) : 0;
    if (read > 0) {
      SeekGuard guard(vma.backup, vma.offset + off);
      vma.backup->read((void *) as_va(pa), read);
    }
  }

  // When the page is writable, to prevent BOOM (2), we must check again.
  // Walking page table is slow, so we want to avoid it in readonly cases (like .text).
  if (auto pte = pte_of((va_t) va_page, root); pte & PTE_V) {
    // Someone has already mapped the page - see the case analysis above.
    // In that case we need to do nothing.
    pfree(pa);
    return true;
  }

  // Now map the page after the read, to avoid BOOM (1).
  os::pmap(pa, va_page, MAP_4KB, flags, root);
  return true;
}

bool map_current(void *va) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return true;

  return map_single(va, pt_root());
}

bool map_current(void *va, pte_t *root) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return true;

  return map_single(va, root);
}

bool map_current(void *from, void *to, bool write) {
  char *p = (char *) from, *q = (char *) to;
  for (p = rounddown<PAGE_SIZE>(p); p < q; p += PAGE_SIZE) {
    int flags = pte_flags(p);
    if (flags == -1 || (flags & PTE_COW && !(flags & PTE_W) && write)) {
      if (!map_single(p, pt_root()))
        return false;
    }
  }
  return true;
}

bool vma_t::mergeable(const vma_t &other) const {
  if (end != other.begin)
    return false;
  if (prot != other.prot || flags != other.flags || backup != other.backup)
    return false;
  auto len = end - begin;
  // Must not have .bss segment, and file must be read contiguously.
  if (backup && (maxread != len || other.offset != offset + len))
    return false;
  return true;
}

vma_t *addrspace::find(va_t addr) const {
  if (cache && cache->begin <= addr && addr < cache->end)
    return cache;

  assert(vmas.root);
  for (node *cur = vmas.root; cur;) {
    int i = 0;
    // This stops at the place where cur->k[i] (i.e. vma begin) >= addr.
    for (; i < cur->count && addr >= cur->k[i]; i++) {
      if (addr < cur->v[i].end)
        return cache = &cur->v[i];
    }

    if (cur->leaf)
      break;

    // Now we must descend to the left child of k[i], i.e. ch[i].
    // All children before it will have a smaller end, because VMAs don't overlap.
    cur = cur->ch[i];
  }
  return nullptr;
}

void addrspace::insert(const vma_t &vma) {
  // We must ensure there's no overlap.
#ifndef NDEBUG
  if (vmas.has_overlap(vma.begin, vma.end)) {
    printk("addrspace: insert: found overlap\n");
    printk("insert: [%p, %p)\nexisting:\n", vma.begin, vma.end);
    for (auto [_, v] : vmas)
      printk("[%p, %p)\n", v.begin, v.end);
    
    panic("overlap");
  }
#endif
  cache = nullptr;
  vmas.insert(vma);
}

va_t addrspace::find_mmap(unsigned long len, va_t hint) const {
  auto va = vmas.find_gap(len, hint ? hint : mmap_begin);
  if (va)
    return va;

  // We might need to start searching from other places.
  printk("mmap: hint = %p, begin = %p, len = %p", hint, mmap_begin, len);
  assert(false && "TODO: find mmap: mmap_begin change not implemented");
}

va_t addrspace::brk(va_t addr) {
  if (addr <= heap_begin)
    return brkp;

  auto rounded = roundup<PAGE_SIZE>(addr);
  if (rounded <= roundup<PAGE_SIZE>(heap_end))
    return brkp = addr;

  // On expand, we first need to check if there's anything on the way.
  if (vmas.has_overlap(heap_end, addr))
    return brkp;

  vma_t *vma = vmas.find(heap_begin);
  vma->end = rounded;
  vmas.update_path(vma->begin);
  heap_end = rounded;
  return brkp = addr;
}

void addrspace::split(va_t addr) {
  vma_t *vmap = find(addr);
  if (!vmap)
    return;

  cache = nullptr;
  auto copy = *vmap;
  vmas.erase(copy.begin);

  auto oldend = copy.end, oldread = copy.maxread;
  size_t firstlen = addr - copy.begin;

  copy.end = addr;
  copy.maxread = min(copy.maxread, firstlen);
  vmas.insert(copy);

  copy.begin = addr;
  copy.end = oldend;
  copy.offset += firstlen;
  copy.maxread = oldread > firstlen ? oldread - firstlen : 0;
  vmas.insert(copy);
}

vma_t::~vma_t() {
  if (backup)
    backup->drop();
}

vma_t::vma_t(uintptr_t begin, uintptr_t end, int prot, int flags): begin(begin), end(end), prot(prot), flags(flags), backup(nullptr), offset(0), maxread(0) { }

vma_t::vma_t(uintptr_t begin, uintptr_t end, int prot, int flags, file *backup, size_t offset, size_t maxread)
  : begin(begin), end(end), prot(prot), flags(flags), backup(backup), offset(offset), maxread(maxread) {
  if (backup)
    backup->ref();
}

vma_t::vma_t(const vma_t &other): begin(other.begin), end(other.end), prot(other.prot), flags(other.flags), backup(other.backup), offset(other.offset), maxread(other.maxread) {
  if (backup)
    backup->ref();
}

vma_t::vma_t(vma_t &&other): begin(other.begin), end(other.end), prot(other.prot), flags(other.flags), backup(other.backup), offset(other.offset), maxread(other.maxread) {
  other.backup = nullptr;
}

vma_t &vma_t::operator=(const vma_t &other) {
  begin = other.begin;
  end = other.end;
  prot = other.prot;
  flags = other.flags;
  backup = other.backup;
  offset = other.offset;
  maxread = other.maxread;
  if (backup)
    backup->ref();
  return *this;
}

void init() {
}

}

