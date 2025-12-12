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

  if (!pcb->vma.has(addr)) {
    // Examine scause to determine.
    int scause; CSRR(scause, scause);
    auto type = scause == 12 ? "execute" : scause == 13 ? "load" : scause == 15 ? "store" : nullptr;
    if (type) {
      va_t sepc = ((trapframe *) tcb->ksp)->sepc;
      printk("Unmapped address %p on %s, requested from instruction %p of process %d. Terminate the process.\n", va, type, sepc, pcb->pid);
    } else
      printk("Unmapped address %p. Terminate the process.\n", va);
    os::terminate(tcb, -127);
    return;
  }
  auto pa = os::pframe();
  const auto &vma = pcb->vma.at(addr);
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
  if (!vma.backup) {
    // It is required that we zero this if we're using an anonymous mmap.
    if (vma.flags & MAP_ANONYMOUS)
      memset(va_page, 0, PAGE_SIZE);
    return;
  }
  
  // `begin` and this address are in the same page.
  // We read from beginning.
  if (vma.begin / PAGE_SIZE == addr / PAGE_SIZE) {
    SeekGuard guard(vma.backup, vma.offset);
    auto off = vma.begin % PAGE_SIZE;
    auto read = min(PAGE_SIZE - off, vma.maxread);
    vma.backup->read((void *) vma.begin, read);
    memset((char*) rounddown<4_kb>(vma.begin), 0, off);
    memset((char*) vma.begin + read, 0, PAGE_SIZE - off - read);
    return;
  }

  // This is in the middle. We read the entire page.
  va_t off = (va_t) va_page - vma.begin;

  // Note that these are unsigned, so a direct subtraction and then max(..., 0) won't work.
  size_t read = off < vma.maxread ? min((size_t) PAGE_SIZE, vma.maxread - off) : 0;

  if (read > 0) {
    SeekGuard guard(vma.backup, vma.offset + off);
    int rd = vma.backup->read((void *) va_page, read);
  }

  if (read < PAGE_SIZE)
    memset((char*) va_page + read, 0, PAGE_SIZE - read);
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
  for (; p < q; p += PAGE_SIZE) {
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

void vmas::split_at(size_t i, uintptr_t addr) {
  vma_t vma = vmas[i];
  if (!(vma.begin < addr && addr < vma.end))
    return;

  vma_t left = vma;
  left.end = addr;

  vma_t right = vma;
  right.begin = addr;
  if (vma.backup) {
    // Adjust offsets for the right piece.
    size_t lsize = left.end - left.begin;
    right.offset = vma.offset + lsize;
    // Adjust bss sizes, if applicable.
    left.maxread = min(left.maxread, lsize);
    right.maxread = max(0ul, vma.maxread - lsize);
  }

  // Replace orig with `left` and insert `right` after it.
  vmas[i] = left;
  vmas.insert(vmas.begin() + i + 1, right);
}

result vmas::merge_at(size_t i) {
  if (i + 1 >= vmas.size())
    return result::failure;
  if (!vmas[i].mergeable(vmas[i + 1]))
    return result::failure;
  
  vmas[i].end = vmas[i + 1].end;
  // If mergeable, then the first segment cannot have .bss.
  if (vmas[i].backup)
    vmas[i].maxread += vmas[i + 1].maxread;
  vmas.erase(vmas.begin() + i + 1);
  return result::success;
}

size_t vmas::find(va_t addr) const {
  size_t low = 0, high = vmas.size();
  while (low < high) {
    size_t mid = (low + high) / 2;
    if (vmas[mid].begin <= addr) {
      if (vmas[mid].end > addr)
        return mid;
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low;
}

// Keep `vmas` sorted.
void vmas::push(const vma_t &vma) {
  auto begin = vma.begin, end = vma.end;
  size_t i = find(begin);

  // We must split if it's like this:
  // prev [=======]
  //          |------- begin
  if (i > 0) {
    const auto &prev = vmas[i - 1];
    if (prev.begin < begin && begin < prev.end) {
      split_at(i - 1, begin);
      i++;
    }
  }

  // Now there are two cases.
  // Case 1.
  //        [========]
  // |----------| end
  //    Here we must split end, and we can break.
  //
  // Case 2.
  //    [====]
  // |---------| end
  //    In this case, we can simply erase the VMA, since it's completely covered.
  while (i < vmas.size() && vmas[i].begin < end) {
    const auto &cur = vmas[i];
    if (cur.end > end) {
      split_at(i, end);
      break;
    }
    if (cur.begin >= begin)
      vmas.erase(vmas.begin() + i);
  }

  // Finally record the new VMA.
  // Note that we don't allow overwriting PT_LOAD or STACK, so we only check for heap here.
  vmas.insert(vmas.begin() + i, vma);
}

bool vmas::has(va_t addr) const {
  auto point = find(addr);
  if (point == vmas.size())
    return false;

  const auto &vma = vmas[point];
  return vma.begin <= addr && addr < vma.end;
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

}

