#include "ptable.h"
#include "../utils/libc.h"

constexpr int PAGE_SIZE = 4096;

namespace {

struct Frame {
  Frame *next;
  unsigned refcnt;
  char w[PAGE_SIZE - 12];
};

Frame *free_head;

// No extra paddings.
static_assert(sizeof(Frame) == PAGE_SIZE);

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
