#include "kalloc.h"
#include "vma.h"
#include "../fdt/fdt.h"
#include "../utils/libc.h"
#include "../instr/leak.h"

using namespace os;

namespace {

struct frame_t {
  pa_t next;
  char w[PAGE_SIZE - 8];
};

pa_t free_head;
size_t physavail, pmtotal;

// No extra paddings.
static_assert(sizeof(frame_t) == PAGE_SIZE);

#ifndef IN_VSCODE
pframe_meta pmmeta[MAX_PA_SIZE / PAGE_SIZE];
#else
// This is mainly for VSCode performance reasons.
// An array of length 4194304 will have dramatic performance drop.
pframe_meta pmmeta[1];
#endif
constexpr auto PM_META_SIZE = sizeof(pmmeta) / sizeof(pframe_meta);

int clz(unsigned x) {
  if (x == 0)
    return 32;

  // Do a binary search.
  int n = 0;
  if ((x & 0xffff0000) == 0) {
    n += 16;
    x <<= 16;
  }
  if ((x & 0xff000000) == 0) {
    n += 8;
    x <<= 8;
  }
  if ((x & 0xf0000000) == 0) {
    n += 4;
    x <<= 4;
  }
  if ((x & 0xc0000000) == 0) {
    n += 2;
    x <<= 2;
  }
  if ((x & 0x80000000) == 0)
    n += 1;
  return n;
}

// The buddy allocator for physical addresses.
class buddy {
public:
  constexpr static int order = 18;

  using address = uintptr_t;
#define nextof(x) (*(address*) (x))
#define prevof(x) (*((address*) (x) + 1))

  address allocate(unsigned total);
  void free(address addr);
  void reserve(address begin, size_t size);

  // These global variables cannot have a constructor. We have to manually initialize them.
  void init(address base, size_t space);

  // Available pages.
  // We currently track **submitted size** rather than the actual size.
  int available() const { return avail; }
  unsigned offset(address addr) const { return (addr - base) / PAGE_SIZE; }
private:
  // These are free lists, recording virtual addresses.
  // For physical allocator, we only need to include KERNEL_OFFSET inside `base`.
  address heads[order];
  address base;
  int avail = 0;

  void push(int i, address element);
  void erase(int i, address element);
} buddy;

void buddy::push(int i, address element) {
  nextof(element) = heads[i];
  prevof(element) = 0;
  if (heads[i])
    prevof(heads[i]) = element;
  
  heads[i] = element;
  pmmeta[offset(element)].order = i;
}

void buddy::erase(int i, address element) {
  auto next = nextof(element), prev = prevof(element);
  if (prev)
    nextof(prev) = next;
  else
    heads[i] = next;
  if (next)
    prevof(next) = prev;
}

buddy::address buddy::allocate(unsigned total) {
  // Find the next buddy that is larger than the current request.
  int level = 32 - clz(total - 1);
  for (int i = level; i < order; i++) {
    if (!heads[i])
      continue;
    
    auto page = heads[i];
    erase(i, page);
    
    // Split the rest of the block, if it is large enough.
    // We're occupying the first half of the block, so the second half is still available.
    while (i-- > level) {
      auto buddy = page + (PAGE_SIZE << i);
      push(i, buddy);
    }

    // Increase reference count.
    auto pos = offset(page);
    assert(pmmeta[pos].refcnt == 0 && "double allocate");
    pmmeta[pos].refcnt = 1;
    pmmeta[pos].order = level;
    avail -= (1 << level);
    return page;
  }
  // Out of memory. TODO: Maybe add swap later.
  panic("out of memory");
}

void buddy::free(buddy::address addr) {
  unsigned pos = offset(addr);
  assert(pmmeta[pos].refcnt != 0 && "double free");
  if (--pmmeta[pos].refcnt)
    return;

  int i = pmmeta[pos].order;
  avail += (1 << i);
  for (; i < order - 1; i++) {
    // Try combining with the buddy.
    auto buddy = base + ((addr - base) ^ (PAGE_SIZE << i));
    
    // The buddy isn't free.
    // In that case, we insert `addr` into the free list of level `i`.
    auto budpos = offset(buddy);
    if (pmmeta[budpos].refcnt != 0 || pmmeta[budpos].order != i)
      break;

    // When the buddy is indeed free, we take it out of the free list.
    // It will be combined with the current block and later inserted to a larger block,
    // by the code above.
    erase(i, buddy);
    // The address must be the start of the pair.
    addr = min(addr, buddy);
    pmmeta[budpos].order = -1;
  }
  
  push(i, addr);
  pmmeta[offset(addr)].order = i;
}

void buddy::reserve(address begin, size_t size) {
  auto end = begin + size;
  while (begin < end) {
    // Find the largest order to reserve for `begin`.
    int level = 0;
    size_t remaining = end - begin;

    for (; level + 1 < order; level++) {
      size_t block_size = PAGE_SIZE << (level + 1);
      if ((begin & (block_size - 1)) != 0)
        break;
      if (block_size > remaining)
        break;
    }

    // We now want to erase a block at `level`.
    // But to do this, we must first ensure we have a block at that level.
    int cur = level;
    auto pos = offset(begin);
    // Find the first level that has a block.
    while (cur < order && heads[cur])
      cur++;
    // Split that block till `level`.
    while (cur > level) {
      erase(cur, begin);
      cur--;

      address buddy = begin + (PAGE_SIZE << cur);
      push(cur, buddy);

      pmmeta[pos].order = cur;
      pmmeta[offset(buddy)].order = cur;
    }

    // Remove the block from free list and mark as allocated.
    erase(level, begin);
    pmmeta[pos].refcnt = 1;
    pmmeta[pos].order = level;
    begin += PAGE_SIZE << level;
  }
}

void buddy::init(address base, size_t space) {
  this->base = base;
  avail = space / PAGE_SIZE;

  auto addr = base;
  memset(heads, 0, sizeof(heads));
  for (int i = 0; i < order; i++) {
    if (!(space & (PAGE_SIZE << i)))
      continue;

    heads[i] = addr;
    nextof(addr) = 0;
    prevof(addr) = 0;
    pmmeta[offset(addr)].order = i;
    addr += (PAGE_SIZE << i);
  }
}

// The B-tree allocator for virtual addresses.
class vm_allocator {
  using page_number = size_t;

  // This is the B-tree order.
  constexpr static int order = 8;

  // The B-tree node allocator, to avoid circular dependency on memory allocation.
  // B-tree node size doesn't depend on allocator.
  class node_allocator {
    using V = os::interval_btree<interval<page_number>, order>::node;
    constexpr static size_t space = 2_mb;
    constexpr static size_t node_size = sizeof(V);
    constexpr static size_t capacity = space / node_size;
    
    char arena[space];
    void *head;
  public:
    node_allocator();
    // This is only called within B-tree, so we can safely assume every allocation is of the size of a node.
    void *allocate(size_t len);
    void free(void*);
  };
  
  os::interval_btree<interval<page_number>, order, node_allocator> map;
  va_t base;
public:
  using address = va_t;

  address allocate(page_number total);
  unsigned free(address addr);
  vm_allocator(address base, page_number size);
};

static_storage<vm_allocator> valloc;

vm_allocator::node_allocator::node_allocator(): head(arena) {
  // Create a free list.
  char *p = arena;
  for (; p < arena + space - node_size; p += node_size)
    nextof(p) = (address) (p + node_size);
  
  nextof(p) = 0;
}

void *vm_allocator::node_allocator::allocate(size_t len) {
  assert(head);
  assert(len == node_size); (void) len;
  auto va = head;
  head = (void *) nextof(va);
  return va;
}

void vm_allocator::node_allocator::free(void *p) {
  nextof(p) = (address) head;
  head = p;
}

vm_allocator::address vm_allocator::allocate(page_number total) {
  page_number begin = map.find_gap(total);
  if (begin == 0) {
    printk("vm: required %d pages, but remaining %d\n", total, pavail());
    panic("vm: out of memory");
  }
  map.insert({ begin, begin + total });
  return base + begin * PAGE_SIZE;
}

unsigned vm_allocator::free(address addr) {
  page_number off = (addr - base) / PAGE_SIZE;
  interval<page_number> *it = map.find(off);
  assert(it != nullptr);
  auto cnt = it->end - it->begin;
  map.erase(off);
  return cnt;
}

vm_allocator::vm_allocator(address base, page_number size): base(base) {
  // Insert placeholders for minimum and maximum.
  // We don't want `0` to be allocatable, as we use 0 to represent OOM.
  map.insert({ 0, 1 });
  map.insert({ size, size + 1 });
}

void *page_malloc(size_t total, int flags) {
  if (!total)
    return nullptr;

#ifdef DEBUG_MEMORY
  // Add two guard pages, one at beginning and one at end.
  size_t actual = total + 2;
#else
  size_t actual = total;
#endif

  auto base = valloc->allocate(actual);
#ifdef DEBUG_MEMORY
  base += PAGE_SIZE;
#endif
  for (size_t i = 0; i < total; ++i) {
    pa_t frame = pframe();
    // Out of physical memory. Free all obtained memory.
    if (!frame) {
      panic("vmalloc: out of memory");
      for (size_t j = 0; j < i; ++j) {
        auto pa = punmap(base + j * PAGE_SIZE, MAP_4KB);
        pfree(*pa);
      }
      valloc->free(base);
      return nullptr;
    }
    pmap(frame, base + i * PAGE_SIZE, MAP_4KB, flags);
  }
  
  return (void *) base;
}

#ifdef DEBUG_MEMORY
// Ignore the two unmapped guard pages.
void page_free(void *va) {
  auto base = (va_t) va;
  auto freed = valloc->free(base - PAGE_SIZE);
  for (size_t i = 0; i < freed - 2; ++i) {
    va_t v = base + i * PAGE_SIZE;
    auto p = punmap(v, MAP_4KB);
    pfree(*p);
  }
}
#else
void page_free(void *va) {
  auto base = (va_t) va;
  auto freed = valloc->free(base);
  for (size_t i = 0; i < freed; ++i) {
    va_t v = base + i * PAGE_SIZE;
    auto p = punmap(v, MAP_4KB);
    pfree(*p);
  }
}
#endif

void mark_reserved() {
  // Look at the /memory node.
  pa_t physbegin, physend;
  char *p = (char *) fdt::pfdt + to_big_endian(fdt::pfdt->off_dt_struct);
  fdt::walk(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    if (strncmp(cdev, "/memory@", 8) != 0)
      return WalkResult::Continue;

    if (strcmp(cprop, "reg") == 0) {
      assert(len == 16); (void) len;
      auto prop = (uint32_t*) property;
      physbegin = (to_big_endian(prop[0]) * 1ull << 32) | to_big_endian(prop[1]);
      physend = physbegin + ((to_big_endian(prop[2]) * 1ull << 32) | to_big_endian(prop[3]));
      return WalkResult::Interrupt;
    }
    return WalkResult::Continue;
  });
  
  // We don't start from `physbegin`, because typically there are already things here.
  // On RISC-V it's for OpenSBI, and on Loongarch that is probably 0x0.
  auto end = (va_t) __kernel_end;
  auto size = min((physend + KERNEL_OFFSET) - end, MAX_PA_SIZE);
  buddy.init(end, size);
  physavail += size / PAGE_SIZE;
  
  for (auto *rsvmap = (fdt::memrsv*) ((char *) fdt::pfdt + to_big_endian(fdt::pfdt->off_mem_rsvmap));; rsvmap++) {
    if (rsvmap->address == 0 && rsvmap->size == 0)
      break;

    auto begin = rsvmap->address + KERNEL_OFFSET;
    auto size = roundup<PAGE_SIZE>(rsvmap->size);
    buddy.reserve(begin, size);
    physavail -= size;
  }
  pmtotal = physavail;
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
static_assert(size_count <= 15);

intrusive_list<slab> slabs[size_count];

bool push_slab(int i) {
  size_t size = sizes[i];
  void *page = page_malloc(1, PTE_RW | PTE_V);
  if (!page)
    return false;

  // Mark the belonging of the page.
  auto pos = off(to_pa(page));
  assert(pos <= PM_META_SIZE);
  pmmeta[pos].type = i;

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

#if (defined(DEBUG_MEMORY_EXPENSIVE) || defined(UNIT_TEST)) && defined(FUNC_INSTRUMENT)
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
  pmmeta[off(p)].refcnt++;
}

pa_t pframe() {
  // Note buddies manage virtual addresses.
  pa_t pa = buddy.allocate(1) - KERNEL_OFFSET;
#if defined(DEBUG_MEMORY)
  memset((void *) as_va(pa), 0xAA, PAGE_SIZE);
#endif
  return pa;
}

void pfree(pa_t pa) {
  if (!pa)
    return;
  assert(pa % PAGE_SIZE == 0);
  
  auto pos = off(pa);
  
#if defined(DEBUG_MEMORY)
  // When this page is about to get freed, poison it.
  if (pmmeta[pos].refcnt == 1)
    memset((void *) as_va(pa), 0xCC, PAGE_SIZE);
#endif

  buddy.free(pa + KERNEL_OFFSET);
}

pa_t pframe_zeroed() {
  auto p = pframe();
  memset((void *) as_va(p), 0, PAGE_SIZE);
  return p;
}

void make_zeroes() {
  auto p = pframe();
  memset((void *) as_va(p), 0, PAGE_SIZE);

  synchronized _(zero::lock);
  auto head = (frame_t *) as_va(p);
  head->next = zero::head;
  zero::head = p;
  zero::len++;
}

pa_t pmalloc(int pagecnt) {
  return buddy.allocate(pagecnt) - KERNEL_OFFSET;
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

  size_t pagecount = os::roundup<PAGE_SIZE>(len) / PAGE_SIZE;
  return page_malloc(pagecount, PTE_RW | PTE_V);
}

void vfree(void *p) {
#if defined(DEBUG_MEMORY) && defined(FUNC_INSTRUMENT)
  leak::record_free(p);
#endif
  if (!p)
    return;

  auto pos = off(to_pa(p));
  // This is managed by slab allocator.
  if (auto type = pmmeta[pos].type) {
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

  // This is managed by the B-tree page allocator.
  page_free(p);
}

void init_kalloc() {
  mark_reserved();
  valloc.construct(VM_BASE, VM_SIZE / PAGE_SIZE);
}

size_t pavail() {
  return buddy.available();
}

size_t ptotal() {
  return pmtotal;
}

size_t off(pa_t pa) {
  return buddy.offset(pa + KERNEL_OFFSET);
}

pframe_meta *inspect_meta() {
  return pmmeta;
}

int refcnt(pa_t pa) {
  return pmmeta[off(pa)].refcnt;
}

}
