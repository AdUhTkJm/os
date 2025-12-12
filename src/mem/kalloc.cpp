#include "ptable.h"
#include "kalloc.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../instr/leak.h"

using namespace os;

// Reserved kernel virtual memory size.
constexpr size_t VM_SIZE = 1_gb;
constexpr va_t VM_BASE = 0xffff'ffff'c000'0000ul;

// The entire physical memory space we're able to manage. QEMU only has 128MB anyway.
// When we enable DEBUG_MEMORY, the meta becomes incredibly large.
#ifdef DEBUG_MEMORY
constexpr va_t MAX_PA_SIZE = 512_mb;
#else
constexpr va_t MAX_PA_SIZE = 2_gb;
#endif

// This amount of 4KB frames from __kernel_base will be managed by
// the free-list allocator, mainly for bootstrapping.
// All other regions will be managed by the bitmap allocator.
constexpr va_t FREE_LIST_SIZE = 0x1000;

namespace {

struct frame_t {
  pa_t next;
  char w[PAGE_SIZE - 8];
};

pa_t free_head;

// No extra paddings.
static_assert(sizeof(frame_t) == PAGE_SIZE);

// 1 for occupied, 0 for free.
os::bitmap<VM_SIZE / PAGE_SIZE> vmmap;
os::bitmap<MAX_PA_SIZE / PAGE_SIZE> pmmap;

uintptr_t physbegin, physend;

struct pframe_meta {
#ifdef DEBUG_MEMORY
  void *alloc_pc;
  // stack::shadow_stack stack;
#endif
  unsigned char refcnt;
};

#ifndef IN_VSCODE
pframe_meta meta[MAX_PA_SIZE / PAGE_SIZE];
#else
// This is mainly for VSCode performance reasons.
// An array of length 4194304 will have dramatic performance drop.
pframe_meta meta[1];
#endif
bool pminit;

// Finds `total` consecutive virtual pages.
template<size_t N, typename T>
size_t find_consecutive(const os::bitmap<N, T> &bitmap, size_t total, size_t from, size_t to) {
  size_t len = 0, start = from;

  for (size_t w = from; w < to; w += bitmap.unit_bits) {
    auto word = bitmap.word(w / bitmap.unit_bits);
    // A whole chunk of zeroes. Add them directly.
    if (word == 0) {
      if (len == 0)
        start = w;
      len += bitmap.unit_bits;
      if (len >= total)
        return start;
      continue;
    }

    // Check each bit separately.
    for (unsigned i = 0; i < bitmap.unit_bits; ++i) {
      if (bitmap[w + i]) {
        len = 0;
        continue;
      }
      if (len == 0)
        start = w + i;
      if (++len == total)
        return start;
    }
  }
  return -1ul;
}

template<size_t T>
size_t find_consecutive(const os::bitmap<T> &bitmap, size_t total, size_t from) {
  size_t pos = find_consecutive(bitmap, total, from, bitmap.size);
  if (pos != -1ul)
    return pos;

  return find_consecutive(bitmap, total, 0, from);
}

void *vm_alloc_pages(size_t total, int flags) {
  static int vm_from = 0;

  if (!total)
    return nullptr;
  size_t index = find_consecutive(vmmap, total, vm_from);
  if (index == -1ul)
    return nullptr;

  uintptr_t base = VM_BASE + index * PAGE_SIZE;

  for (size_t i = 0; i < total; ++i) {
    pa_t frame = pframe();
    // Out of physical memory. Free all obtained memory.
    if (!frame) {
      for (size_t j = 0; j < i; ++j) {
        auto pa = punmap(base + j * PAGE_SIZE, MAP_4KB);
        pfree(*pa);
        vmmap[index + j] = 0;
      }
      return nullptr;
    }
    pmap(frame, base + i * PAGE_SIZE, MAP_4KB, flags);
    vmmap[index + i] = 1;
  }
  // Round-robin allocate.
  vm_from += total;
  return (void *) base;
}

void vm_free_pages(void *va, size_t total) {
  auto base = (uintptr_t) va;
  size_t index = (base - VM_BASE) / PAGE_SIZE;
  for (size_t i = 0; i < total; ++i) {
    uintptr_t v = base + i * PAGE_SIZE;
    auto p = punmap(v, MAP_4KB);
    pfree(*p);
    vmmap[index + i] = 0;
  }
}

void mark_reserved() {
  // Moreover, look at the /memory node.
  char *p = (char *) fdt::pfdt + to_big_endian(fdt::pfdt->off_dt_struct);
  fdt::walk(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    if (strncmp(cdev, "/memory@", 8) != 0)
      return WalkResult::Continue;

    if (strcmp(cprop, "reg") == 0) {
      assert(len == 16);
      auto prop = (uint32_t*) property;
      physbegin = (to_big_endian(prop[0]) * 1ull << 32) | to_big_endian(prop[1]);
      physend = physbegin + ((to_big_endian(prop[2]) * 1ull << 32) | to_big_endian(prop[3]));
      
      return WalkResult::Interrupt;
    }
    return WalkResult::Continue;
  });

  auto rsv = fdt::reserved();
  // Don't touch the space of the free-list allocator.
  // Also don't touch the kernel itself.
  auto endpa = to_pa(__kernel_end);
  rsv.push_back({ 0x8000'0000, endpa - 0x8000'0000 });
  rsv.push_back({ endpa, FREE_LIST_SIZE * PAGE_SIZE });

  for (const auto &[begin, size] : rsv) {
    auto start_page = (begin - physbegin) / PAGE_SIZE;
    auto end_page = start_page + roundup<PAGE_SIZE>(size) / PAGE_SIZE;
    pmmap.set(start_page, end_page);
  }
}

}

namespace os {

void pincref(pa_t p) {
  meta[p / PAGE_SIZE].refcnt++;
}

pa_t pframe() {
  if (!free_head && !pminit)
    panic("out of memory");

  size_t pa;
  if (free_head) {
    // Free-list part.
    pa = free_head;
    frame_t *head = (frame_t *) as_va(free_head);
    free_head = (pa_t) head->next;
  } else {
    // Bitmap part.
    static int pm_from = 0;
    size_t index = find_consecutive(pmmap, 1, pm_from);
    if (index == -1ul)
      panic("out of memory");

    pmmap[index] = 1;
    pm_from = index + 1;
    pa = index * PAGE_SIZE + physbegin;
  }

  auto pos = pa / PAGE_SIZE;
  meta[pos].refcnt++;
#ifdef DEBUG_MEMORY
  meta[pos].alloc_pc = __builtin_return_address(0);
  // os::stack::copy(&meta[pos].stack);
  memset((void *) as_va(pa), 0xAA, PAGE_SIZE);
#endif
  return pa;
}

pa_t pframe_zeroed() {
  auto p = pframe();
  memset((void *) as_va(p), 0, PAGE_SIZE);
  return p;
}

void pfree(pa_t p) {
  if (!p)
    return;
  
  auto pos = p / PAGE_SIZE;
#ifdef DEBUG_MEMORY
  if (meta[pos].refcnt == 0) {
    printk("%p double-freed (previous allocation %p).\n", p, meta[pos].alloc_pc);
    // os::stack::dump(meta[pos].stack);
    panic("memory: pfree");
  }
  memset((void *) as_va(p), 0xCC, PAGE_SIZE);
#endif
  
  if (--meta[pos].refcnt > 0)
    return;

  // This region is managed by free list allocator.
  const auto end = (pa_t) __kernel_end - KERNEL_OFFSET;
  if (p >= end && p < end + FREE_LIST_SIZE * PAGE_SIZE) {
    auto *frame = (frame_t *) as_va(p);
    frame->next = free_head;
    free_head = to_pa(frame);
    return;
  }

  // This is managed by the bitmap allocator.
  auto base = (uintptr_t) p;
  auto index = (base - physbegin) / PAGE_SIZE;
  pmmap[index] = 0;
}

pa_t pmalloc(int pagecnt) {
  if (!pminit)
    return 0;
  if (pagecnt == 1)
    return pframe();

  // We always search from the beginning. (The round-robin won't be in sync with pframe().)
  auto index = find_consecutive(pmmap, pagecnt, 0);
  if (index == -1ul)
    return 0;
  pmmap.set(index, index + pagecnt);
  for (size_t i = index; i < index + pagecnt; i++)
    meta[i].refcnt++;
  return index * PAGE_SIZE + physbegin;
}

void *vmalloc_impl(size_t len) {
  size_t pagecount = os::roundup<PAGE_SIZE>(len + sizeof(size_t)) / PAGE_SIZE;
  size_t *p = (size_t *) vm_alloc_pages(pagecount, PTE_RW | PTE_V);
  *p = pagecount;
  return p + 1;
}

void vfree(void *p) {
#ifdef FUNC_INSTRUMENT
  leak::record_free(p);
#endif
  if (!p)
    return;
  auto q = (size_t *) p;
  [[unlikely]] while (!q[-1])
    q--;
  size_t *meta = q - 1;
  size_t pagecount = *meta;
  vm_free_pages(meta, pagecount);
}

void init_bitmap_kalloc() {
  pminit = true;
  mark_reserved();
}

void init_freelist_kalloc() {
  // Grab 16MB of memory. The linker script guarantees alignment.
  pa_t begin = (pa_t) __kernel_end - KERNEL_OFFSET;
  pa_t end = begin + FREE_LIST_SIZE * PAGE_SIZE;

  for (pa_t p = begin; p != end; p += PAGE_SIZE)
    ((frame_t *) as_va(p))->next = p + PAGE_SIZE;
  
  ((frame_t *) as_va(end) - 1)->next = 0;
  free_head = begin;
}

}
