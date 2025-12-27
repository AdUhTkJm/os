#include "vma.h"
#include "../proc/schedule.h"

namespace os::vma {

void map_single(void *va, pte_t *root) {
  EnableAccessToUserMemory enabler;
  auto tcb = active();
  auto pcb = tcb->pcb;

  va_t addr = (va_t) va;
  auto va_page = rounddown<4_kb>(va);
  int origflags = pte_flags(va_page);

  auto vmap = pcb->vma.find(addr);
  if (!vmap) {
    // Examine scause to print a better debug message. This is mainly for RISC-V.
#ifdef RV
    int scause; CSRR(scause, scause);
    auto type = scause == 12 ? "execute" : scause == 13 ? "load" : scause == 15 ? "store" : nullptr;
    if (type) {
      va_t sepc = ((trapframe *) tcb->ksp)->sepc;
      printk("Unmapped address %p on %s, requested from instruction %p of process %d. Terminate the process.\n", va, type, sepc, pcb->pid);
    } else
#endif
      printk("Unmapped address %p. Terminate the process.\n", va);
    os::terminate(tcb, -127);
    return;
  }
  const auto &vma = *vmap;

  // We use zero-page optimization here. For MAP_ANONYMOUS (i.e. no backup file),
  // we can allocate a zero-page and copy-on-write.
  pa_t pa = (vma.flags & MAP_SHARED)
    ? to_pa((*vma.backup->node()->cache)[(addr - vma.begin + vma.offset) / PAGE_SIZE].data)
    : os::pframe_zeroed();
  
  int flags = PTE_V | PTE_U;
  if (vma.prot & PROT_EXEC) flags |= PTE_X;
  if (vma.prot & PROT_READ) flags |= PTE_R;
  if (vma.prot & PROT_WRITE) flags |= PTE_W;

  if (origflags != -1 && (origflags & PTE_COW)) {
    // This is a copy-on-write segment. We copy the original contents.
    memcpy((void *) as_va(pa), va_page, PAGE_SIZE);
    // The original pa must be freed.
    pfree(to_pa(va_page));
    // Remap the memory and let it point to the new pa.
    os::pmap(pa, va_page, MAP_4KB, flags | PTE_W, root);
    return;
  }

  // Temporarily map with write permission, if we need to copy into it.
  // Note that it is possible that `prot == 0`. In this case, we need to
  // grab both PTE_R and PTE_W; otherwise RISC-V complains about this.
  bool mustwrite = vma.backup || (vma.flags & MAP_ANONYMOUS);
  int tempflags = mustwrite ? flags | PTE_RW : flags;
  os::pmap(pa, va_page, MAP_4KB, tempflags, root);

  // Take back the write permission on exit.
  const auto &finisher = [&]() {
    if (tempflags != flags)
      os::pmap(pa, va_page, MAP_4KB, flags, root);
  };
  struct takeback {
    decltype(finisher) f;
    takeback(decltype(finisher) f): f(f) {}
    ~takeback() { f(); }
  } _takeback(finisher);

  // Copy the contents if it exists.
  if (!vma.backup)
    // It is required that we zero this if we're using an anonymous mmap.
    return;
  
  // `begin` and this address are in the same page.
  // We read from beginning.
  if (vma.begin / PAGE_SIZE == addr / PAGE_SIZE) {
    SeekGuard guard(vma.backup, vma.offset);
    auto off = vma.begin % PAGE_SIZE;
    auto read = min(PAGE_SIZE - off, vma.maxread);
    vma.backup->read((void *) vma.begin, read);
    return;
  }

  // This is in the middle. We read the entire page.
  va_t off = (va_t) va_page - vma.begin;

  // Note that these are unsigned, so a direct subtraction and then max(..., 0) won't work.
  ssize_t read = off < vma.maxread ? min((size_t) PAGE_SIZE, vma.maxread - off) : 0;

  if (read > 0) {
    SeekGuard guard(vma.backup, vma.offset + off);
    vma.backup->read((void *) va_page, read);
  }
}

void map_current(void *va) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return;

  return map_single(va, pt_root());
}

void map_current(void *va, pte_t *root) {
  int flags = pte_flags(va);
  // Don't remap.
  if (flags != -1 && !(flags & PTE_COW))
    return;

  return map_single(va, root);
}

void map_current(void *from, void *to, bool write) {
  char *p = (char *) from, *q = (char *) to;
  for (p = rounddown<PAGE_SIZE>(p); p < q; p += PAGE_SIZE) {
    int flags = pte_flags(p);
    if (flags == -1 || (flags & PTE_COW && !(flags & PTE_W) && write))
      map_single(p, pt_root());
  }
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
  assert(false && "TODO: find mmap: mmap_begin change not implemented");
}

va_t addrspace::brk(va_t addr) {
  addr = roundup<PAGE_SIZE>(addr);
  if (addr <= heap_begin)
    return heap_end;

  if (addr == heap_end)
    return heap_end;

  // On expand, we first need to check if there's anything on the way.
  if (addr >= heap_end && vmas.has_overlap(heap_end, addr))
    return heap_end;

  vma_t *vma = vmas.find(heap_begin);
  vma->end = addr;
  vmas.update_path(vma->begin);
  return heap_end = addr;
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

