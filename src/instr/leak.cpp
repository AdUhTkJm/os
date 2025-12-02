// leak_tracker.cpp
#include "leak.h"
#include "../lock/lock.h"

// Config.
#define MAX_THREADS 1
#define SHADOW_DEPTH 32
#define NBUCKETS 4096
#define MAX_RECORDS 16384

namespace {

struct shadow_stack {
  void *frames[SHADOW_DEPTH];
  int top;
};

static shadow_stack stack;

struct alloc_record {
  void *ptr;
  size_t size;
  int depth;
  void *frames[SHADOW_DEPTH];
  alloc_record *next;
  bool used;
};

static alloc_record pool[MAX_RECORDS];
static alloc_record *alloc_table[NBUCKETS];
os::spinlock table_lock;

// We're implementing a basic hash table.
// We don't use the existing os::hashmap, because we can't call
// the kernel allocator. Otherwise, it will result in recursion.

[[gnu::no_instrument_function]] alloc_record *pool_alloc(void) {
  for (int i = 0; i < MAX_RECORDS; ++i) {
    if (!pool[i].used) {
      pool[i].used = true;
      pool[i].next = nullptr;
      return pool + i;
    }
  }
  return nullptr;
}

[[gnu::no_instrument_function]] void pool_free(alloc_record *r) {
  r->used = false;
}

[[gnu::no_instrument_function]] size_t bucket(void *p) {
  uintptr_t v = (uintptr_t)p;
  return (v >> 4) & (NBUCKETS - 1);
}

// A basic symbol table by preprocessor and build script.
struct Symbol {
  uintptr_t addr;
  const char* name;
};

// By construction, this is sorted. So when lookup, we can use a binary search.
const Symbol symbols[] = {
#include "../../build/instr/symtbl.inc"
};

constexpr size_t symcnt = sizeof(symbols) / sizeof(Symbol);

const char* lookup_symbol(uintptr_t pc) {
  int l = 0, r = symcnt;

  while (l + 1 < r) {
    int m = (l + r) / 2;
    if (symbols[m].addr <= pc)
      l = m;
    else
      r = m;
  }

  if (symbols[l].addr <= pc)
    return symbols[l].name;

  return "<unknown>";
}

}

extern "C" void __cyg_profile_func_enter(void *this_fn, void* /*call_site*/) {
  if (stack.top < SHADOW_DEPTH)
    stack.frames[stack.top++] = this_fn;
}

extern "C" void __cyg_profile_func_exit(void *, void *) {
  if (stack.top > 0)
    stack.top--;
}

namespace os::leak {

void record_alloc(void *ptr, size_t size) {
  if (!ptr) return;

  // Snapshot the shadow stack.
  void *frames[SHADOW_DEPTH];
  int depth = os::min(SHADOW_DEPTH, stack.top);
  for (int i = 0; i < depth; ++i)
    frames[i] = stack.frames[i];

  table_lock.acquire();
  alloc_record *r = pool_alloc();
  if (!r) {
    // We don't have a pool.
    table_lock.release();
    return;
  }
  r->ptr = ptr;
  r->size = size;
  r->depth = depth;
  for (int i = 0; i < depth; i++)
    r->frames[i] = frames[i];

  size_t b = bucket(ptr);
  r->next = alloc_table[b];
  alloc_table[b] = r;
  table_lock.release();
}

void record_free(void *ptr) {
  if (!ptr)
    return;
  table_lock.acquire();
  size_t b = bucket(ptr);
  alloc_record **p = &alloc_table[b];
  while (*p) {
    if ((*p)->ptr == ptr) {
      alloc_record *victim = *p;
      *p = victim->next;
      pool_free(victim);
      table_lock.release();
      return;
    }
    p = &(*p)->next;
  }
  table_lock.release();
}

void dump() {
  uint64_t total = 0;
  table_lock.acquire();
  printk("=== Leak dump ===\n");
  for (int b = 0; b < NBUCKETS; b++) {
    for (alloc_record *r = alloc_table[b]; r; r = r->next) {
      total += r->size;
      printk("LEAK ptr = %p size = %ld\n",
           r->ptr, r->size);
      for (int i = r->depth - 1; i >= 0; i--) {
        printk("  #%d: %p (%s)\n", i, r->frames[i], lookup_symbol((uintptr_t) r->frames[i]));
      }
    }
  }
  printk("Leaked total: %ld\n", (unsigned long long) total);
  table_lock.release();
}

}
