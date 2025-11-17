#include "ptable.h"
#include "kalloc.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"

// Reserved kernel virtual memory size.
constexpr size_t VM_SIZE = 1 << 30;
constexpr va_t VM_BASE = 0xff0000000ul;

// The entire physical memory space we're able to manage. (16 GB.)
constexpr va_t MAX_PA_SIZE = 1ull << 34;
// This amount of 4KB frames from __kernel_base will be managed by
// the free-list allocator, mainly for bootstrapping.
// All other regions will be managed by the bitmap allocator.
constexpr va_t FREE_LIST_SIZE = 0x1000;

using namespace os;

namespace {

struct Frame {
  Frame *next;
  char w[PAGE_SIZE - 8];
};

Frame *free_head;

// No extra paddings.
static_assert(sizeof(Frame) == PAGE_SIZE);

// 1 for occupied, 0 for free.
os::bitmap<VM_SIZE / PAGE_SIZE> vmmap;
os::bitmap<MAX_PA_SIZE / PAGE_SIZE> pmmap;
bool pminit;

// Finds `total` consecutive virtual pages.
template<size_t N, typename T>
size_t find_consecutive(const os::bitmap<N, T> bitmap, size_t total, size_t from, size_t to) {
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
size_t find_consecutive(const os::bitmap<T>& bitmap, size_t total, size_t from) {
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
    void *frame = pframe();
    // Out of physical memory. Free all obtained memory.
    if (!frame) {
      for (size_t j = 0; j < i; ++j) {
        auto [pa, status] = punmap(base + j * PAGE_SIZE, MAP_4KB);
        pfree((void *) pa);
        vmmap[index + j] = 0;
      }
      return nullptr;
    }
    pmap((pa_t) frame, base + i * PAGE_SIZE, MAP_4KB, flags);
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
    pfree((void *) p);
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

void build_page_list() {
  // Grab 64MB of memory. The linker script guarantees alignment.
  //
  // We use __builtin_assume_aligned, or otherwise the final
  // `(end - 1)->next = nullptr` will become 8 `sb`s rather than a 
  // single `sd`.
  Frame *begin = (Frame*)__builtin_assume_aligned(__kernel_end, 8);
  Frame *end = begin + FREE_LIST_SIZE;

  for (Frame *p = begin; p != end; p++)
    p->next = p + 1;
  
  (end - 1)->next = nullptr;
  free_head = begin;
}

namespace os {

void *pframe() {
  if (!free_head && !pminit)
    panic("out of memory");
  // Free-list part.
  if (free_head) {
    Frame *result = free_head;
    free_head = free_head->next;
    return result;
  }

  // Bitmap part.
  static int pm_from = 0;
  size_t index = find_consecutive(vmmap, 1, pm_from);
  if (index == -1ul)
    return nullptr;

  vmmap[index] = 1;
  pm_from++;
  return (void *) (index * PAGE_SIZE);
}

void pfree(void *p) {
  // This region is managed by free list allocator.
  if (p >= __kernel_end && p <= __kernel_end + FREE_LIST_SIZE) {
    auto *frame = (Frame *)p;
    frame->next = free_head;
    free_head = frame;
  }

  // This is managed by the bitmap allocator.
  auto base = (uintptr_t) p;
  pmmap[base / PAGE_SIZE] = 0;
}

void *vmalloc(size_t len) {
  size_t pagecount = os::roundup<PAGE_SIZE>(len + sizeof(size_t)) / PAGE_SIZE;
  size_t *p = (size_t *) vm_alloc_pages(pagecount, PTE_RW | PTE_V);
  *p = pagecount;
  return p + 1;
}

void vfree(void *p) {
  if (!p)
    return;
  void *meta = (size_t *) p - 1;
  size_t pagecount = *(size_t *) meta;
  vm_free_pages(meta, pagecount);
}

void init_pm_allocator() {
  pminit = true;
  mark_reserved();
}

}
