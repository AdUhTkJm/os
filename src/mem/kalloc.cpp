#include "ptable.h"
#include "kalloc.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"

// Reserved kernel virtual memory size.
constexpr size_t VM_SIZE = 1 << 30;
constexpr va_t VM_BASE = 0xffff'ffff'c000'0000ul;

// The entire physical memory space we're able to manage. (16 GB.)
constexpr va_t MAX_PA_SIZE = 1ull << 34;
// This amount of 4KB frames from __kernel_base will be managed by
// the free-list allocator, mainly for bootstrapping.
// All other regions will be managed by the bitmap allocator.
constexpr va_t FREE_LIST_SIZE = 0x1000;

using namespace os;

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
struct pframe_meta {
  unsigned char refcnt;
} meta[MAX_PA_SIZE / PAGE_SIZE];
bool pminit;

// Finds `total` consecutive virtual pages.
template<size_t N, typename T>
size_t find_consecutive(const os::bitmap<N, T> &bitmap, size_t total, size_t from, size_t to) {
  size_t len = 0, start = from;

  for (size_t w = from; w < to / bitmap.unit_bits; w += bitmap.unit_bits) {
    auto word = bitmap.word(w / bitmap.unit_bits);
    // A whole chunk of zeroes. Skip them directly.
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
        auto [pa, status] = punmap(base + j * PAGE_SIZE, MAP_4KB);
        pfree(pa);
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
    auto [p, status] = punmap(v, MAP_4KB);
    pfree(p);
    vmmap[index + i] = 0;
  }
}

void mark_reserved() {
  auto rsv = fdt::reserved();
  // Don't touch the space of the free-list allocator.
  rsv.push_back({ (pa_t) __kernel_end, FREE_LIST_SIZE * PAGE_SIZE });

  for (const auto &[begin, size] : rsv) {
    auto start_page = begin / PAGE_SIZE;
    auto end_page = start_page + roundup<PAGE_SIZE>(size) / PAGE_SIZE;
    pmmap.clear(start_page, end_page);
  }
}

}

namespace os {

pa_t pframe() {
  if (!free_head && !pminit)
    panic("out of memory");
  // Free-list part.
  if (free_head) {
    pa_t pa = free_head;
    frame_t *head = (frame_t *) as_va(free_head);
    free_head = (pa_t) head->next;
    meta[pa / PAGE_SIZE].refcnt++;
    return pa;
  }

  // Bitmap part.
  static int pm_from = 0;
  size_t index = find_consecutive(vmmap, 1, pm_from);
  if (index == -1ul)
    return 0;

  vmmap[index] = 1;
  pm_from++;
  meta[index].refcnt++;
  return index * PAGE_SIZE;
}

void pincref(pa_t p) {
  meta[p / PAGE_SIZE].refcnt++;
}

void pfree(pa_t p) {
  if (--meta[p / PAGE_SIZE].refcnt > 0)
    return;

  // This region is managed by free list allocator.
  if (p >= (pa_t) __kernel_end && p <= (pa_t) __kernel_end + FREE_LIST_SIZE) {
    auto *frame = (frame_t *) as_va(p);
    frame->next = free_head;
    free_head = to_pa(frame);
  }

  // This is managed by the bitmap allocator.
  auto base = (uintptr_t) p;
  pmmap[base / PAGE_SIZE] = 0;
}

void *vmalloc_impl(size_t len) {
  size_t pagecount = os::roundup<PAGE_SIZE>(len + sizeof(size_t)) / PAGE_SIZE;
  size_t *p = (size_t *) vm_alloc_pages(pagecount, PTE_RW | PTE_V);
  *p = pagecount;
  return p + 1;
}

void vfree(void *p) {
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
  // Grab 64MB of memory. The linker script guarantees alignment.
  //
  // We use __builtin_assume_aligned, or otherwise the final
  // `(end - 1)->next = nullptr` will become 8 `sb`s rather than a 
  // single `sd`.
  pa_t begin = to_pa(__kernel_end);
  pa_t end = begin + FREE_LIST_SIZE * PAGE_SIZE;

  for (pa_t p = begin; p != end; p += PAGE_SIZE)
    ((frame_t *) as_va(p))->next = p + PAGE_SIZE;
  
  ((frame_t *) as_va(end - 1))->next = 0;
  free_head = begin;
}

}
