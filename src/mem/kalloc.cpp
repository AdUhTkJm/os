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
#if defined(DEBUG_MEMORY) && defined(FUNC_INSTRUMENT)
constexpr va_t MAX_PA_SIZE = 128_mb;
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
// Note that vmmap start from a base of `VM_BASE`;
// pmmap and meta start from a base of `physbegin`.
os::bitmap<VM_SIZE / PAGE_SIZE> vmmap;
os::bitmap<MAX_PA_SIZE / PAGE_SIZE> pmmap;

uintptr_t physbegin, physend;

struct pframe_meta {
  // 0 for normal memory; non-zero value `i` for slabs[i].
  unsigned char type;
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

#ifdef DEBUG_MEMORY
  // Add two guard pages, one at beginning and one at end.
  size_t actual = total + 2;
#else
  size_t actual = total;
#endif

  size_t index = find_consecutive(vmmap, actual, vm_from);
  if (index == -1ul)
    return nullptr;

  uintptr_t base = VM_BASE + index * PAGE_SIZE;
#ifdef DEBUG_MEMORY
  base += PAGE_SIZE;
#endif

  vmmap.set(index, index + actual);

  for (size_t i = 0; i < total; ++i) {
    pa_t frame = pframe();
    // Out of physical memory. Free all obtained memory.
    if (!frame) {
      for (size_t j = 0; j < i; ++j) {
        auto pa = punmap(base + j * PAGE_SIZE, MAP_4KB);
        pfree(*pa);
        vmmap[index + j] = 0;
      }
      vmmap.clear(index, index + actual);
      return nullptr;
    }
    pmap(frame, base + i * PAGE_SIZE, MAP_4KB, flags);
  }
  
  // Round-robin allocate.
  vm_from += actual;
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
  auto endpa = (pa_t) (__kernel_end - KERNEL_OFFSET);
  rsv.push_back({ 0x8000'0000, endpa - 0x8000'0000 });
  rsv.push_back({ endpa, FREE_LIST_SIZE * PAGE_SIZE });

  for (const auto &[begin, size] : rsv) {
    auto start_page = (begin - physbegin) / PAGE_SIZE;
    auto end_page = start_page + roundup<PAGE_SIZE>(size) / PAGE_SIZE;
    pmmap.set(start_page, end_page);
  }
}

struct slab : intrusive_list_node<slab> {
  // The first free object.
  void *head;
  // We have padding anyway, so 8 bytes and 2 bytes are the same here.
  size_t count;
};

constexpr size_t vslab(int denom) {
  return rounddown<16>((PAGE_SIZE - sizeof(slab)) / denom);
}

// Except for 8, all other values must be multiples of 16.
constexpr size_t sizes[] = {
  8, 16, 32, 64, 128,
  vslab(12), vslab(6), vslab(5),
  vslab(4), vslab(3), vslab(2)
};
constexpr size_t size_count = sizeof(sizes) / sizeof(size_t);

intrusive_list<slab> slabs[size_count];

bool push_slab(int i) {
  size_t size = sizes[i];
  void *page = vm_alloc_pages(1, PTE_RW | PTE_V);
  if (!page)
    return false;

  // Mark the belonging of the page.
  auto pos = (to_pa(page) - physbegin) / PAGE_SIZE;
  assert(pos <= sizeof(meta) / sizeof(pframe_meta));
  meta[pos].type = i;

  slab *slb = (slab *) page;
  slb->head = (void *) ((va_t) page + sizeof(slab));
  slb->count = (PAGE_SIZE - sizeof(slab)) / size;

  // Build the internal freelist.
  auto cur = (va_t) slb->head;
  for (size_t j = 0; j < slb->count - 1; j++) {
    *(void **) cur = (void *) (cur + size);
    cur += size;
  }
  *(void **) cur = nullptr;

  slabs[i].push_back(slb);
  return true;
}

void *slab_malloc(size_t len) {
  unsigned i = 0;
  for (; i < size_count; i++) {
    if (len <= sizes[i])
      break;
  }
  // Larger lengths should be handled directly by a page allocator.
  assert(i != size_count);

  // Create a new slab if necessary.
  if (slabs[i].empty()) {
    if (!push_slab(i))
      return nullptr;
  }

  slab *cur = slabs[i].front();
  void *mem = cur->head;

  // Pop one element from the free list.
  cur->head = *(void **) mem;
  if (--cur->count == 0)
    slabs[i].pop_front();

  return mem;
}

void slab_free(void *p, int i) {
  slab *slb = (slab *) rounddown<PAGE_SIZE>(p);
  // Put the slab back.
  if (!slb->count++)
    slabs[i].push_back(slb);

  // Push one element to the free list.
  *(void **) p = slb->head;
  slb->head = p;
}

#ifdef DEBUG_MEMORY
#  define CANARY_BEGIN 0x12345678
#  define CANARY_END   0xfedcba90
#endif

}

#if defined(DEBUG_MEMORY) && defined(FUNC_INSTRUMENT)
[[gnu::no_instrument_function]] void check_slab_freelist() {
  for (unsigned i = 0; i < size_count; i++) {
    for (auto slb = slabs[i].head; slb; slb = slb->next) {
      va_t start = (va_t) slb + sizeof(slab);
      va_t end = (va_t) slb + PAGE_SIZE;

      for (void *cur = slb->head; cur; cur = *(void **) cur) {
        va_t addr = (va_t)cur;
        assert(addr >= start && addr < end);
        assert((addr - start) % sizes[i] == 0);
      }
    }
  }
}
#endif

namespace os {

void pincref(pa_t p) {
  size_t pos = (p - physbegin) / PAGE_SIZE;
  assert(pos < sizeof(meta) / sizeof(pframe_meta));
  meta[pos].refcnt++;
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

  auto pos = (pa - physbegin) / PAGE_SIZE;
  assert(pos < sizeof(meta) / sizeof(pframe_meta));
  meta[pos].refcnt++;
#if defined(DEBUG_MEMORY)
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
  assert(p % PAGE_SIZE == 0);
  
  auto pos = (p - physbegin) / PAGE_SIZE;
  assert(pos < sizeof(meta) / sizeof(pframe_meta));
#if defined(DEBUG_MEMORY)
  if (meta[pos].refcnt == 0) {
    printk("%p double-freed\n", p);
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
    free_head = p;
    return;
  }

  // This is managed by the bitmap allocator.
  auto base = (uintptr_t) p;
  auto index = (base - physbegin) / PAGE_SIZE;
  pmmap[index] = 0;
}

pa_t pmalloc(int pagecnt) {
  if (!pminit)
    panic("pmalloc: bitmap allocator uninitialized");
  if (pagecnt == 1)
    return pframe();

  // We always search from the beginning. (The round-robin won't be in sync with pframe().)
  auto index = find_consecutive(pmmap, pagecnt, 0);
  if (index == -1ul)
    panic("pmalloc: out of memory");
  pmmap.set(index, index + pagecnt);
  assert(index + pagecnt < sizeof(meta) / sizeof(pframe_meta));
  for (size_t i = index; i < index + pagecnt; i++)
    meta[i].refcnt++;
  return index * PAGE_SIZE + physbegin;
}

void *vmalloc_impl(size_t len) {
#ifdef DEBUG_MEMORY
  if (len + 24 <= sizes[size_count - 1]) {
    va_t mem = (va_t) slab_malloc(len + 24);
    *(unsigned long *) mem = len;
    *(unsigned long *) (mem + 8) = CANARY_BEGIN;
    *(unsigned long *) (mem + 16 + len) = CANARY_END;
    return (void *) (mem + 16);
  }
#else
  if (len <= sizes[size_count - 1])
    return slab_malloc(len);
#endif

  size_t pagecount = os::roundup<PAGE_SIZE>(len + sizeof(size_t)) / PAGE_SIZE;
  size_t *p = (size_t *) vm_alloc_pages(pagecount, PTE_RW | PTE_V);
  *p = pagecount;
  return p + 1;
}

void vfree(void *p) {
#if defined(DEBUG_MEMORY) && defined(FUNC_INSTRUMENT)
  leak::record_free(p);
#endif
  if (!p)
    return;
  auto pos = (to_pa(p) - physbegin) / PAGE_SIZE;
  assert(pos < sizeof(meta) / sizeof(pframe_meta));
  if (auto type = meta[pos].type) {
#ifdef DEBUG_MEMORY
    if (*(unsigned long *) ((va_t) p - 8) != CANARY_BEGIN)
      panic("vfree: corrupted begin");
    size_t len = *(unsigned long *) ((va_t) p - 16);
    if (*(unsigned long *) ((va_t) p + len) != CANARY_END)
      panic("vfree: corrupted end");
    slab_free((char *) p - 16, type);
#else
    slab_free(p, type);
#endif
    return;
  }

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
