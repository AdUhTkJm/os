#include "ptable.h"
#include "kalloc.h"
#include "../utils/libc.h"

constexpr int PAGE_SIZE = 4096;
// Reserved kernel virtual memory size.
constexpr size_t VM_SIZE = 1 << 30;
constexpr va_t VM_BASE = 0xff0000000ul;

namespace {

struct Frame {
  Frame *next;
  unsigned refcnt;
  char w[PAGE_SIZE - 12];
};

Frame *free_head;

// No extra paddings.
static_assert(sizeof(Frame) == PAGE_SIZE);

// 1 for occupied, 0 for free.
os::bitmap<VM_SIZE / PAGE_SIZE> vmmap;

// Finds `total` consecutive virtual pages.
size_t vm_find_range(size_t total) {
  size_t len = 0, start = 0;
  for (size_t i = 0; i < vmmap.size; ++i) {
    if (vmmap[i]) {
      len = 0;
      continue;
    }
    if (len == 0)
      start = i;
    if (++len == total)
      return start;
  }
  return -1ul;
}

void *vm_alloc_pages(size_t total, int flags) {
  os::TLBRefreshGuard guard;

  if (!total)
    return nullptr;
  size_t index = vm_find_range(total);
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
    vmmap[index + i] = 0;
  }
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

}

C void build_pagelist() {
  // Grab 32MB of memory. The linker script guarantees alignment.
  //
  // We use __builtin_assume_aligned, or otherwise the final
  // `(end - 1)->next = nullptr` will become 8 `sb`s rather than a 
  // single `sd`.
  Frame *begin = (Frame*)__builtin_assume_aligned(__kernel_end, 8);
  Frame *end = begin + 0x2000;

  for (Frame *p = begin; p != end; p++) {
    p->next = p + 1;
    p->refcnt = 0;
  }
  (end - 1)->next = nullptr;
  free_head = begin;
}

C void *pframe() {
  if (!free_head)
    panic("out of memory");

  Frame *result = free_head;
  result->refcnt++;
  free_head = free_head->next;
  return result;
}

C void pfree(void *p) {
  auto *frame = (Frame *)p;
  if (!--frame->refcnt) {
    frame->next = free_head;
    free_head = frame;
  }
}

C void *vmalloc(size_t len) {
  size_t pagecount = os::roundup<PAGE_SIZE>(len + sizeof(size_t)) / PAGE_SIZE;
  size_t *p = (size_t *) vm_alloc_pages(pagecount, PTE_RW | PTE_V);
  *p = pagecount;
  return p + 1;
}

C void vfree(void *p) {
  size_t pagecount = *((size_t *) p - 1);
  vm_free_pages(p, pagecount);
}
