#ifndef NDEBUG
#include "../mem/vma.h"
#include "../utils/stl/intervaltree.h"

using os::vma::vma_t;

void test_btree() {
  os::interval_btree<vma_t, 8> map;

  // Insert.
  for (int i = 1; i < 30; i++) {
    map.insert(vma_t(i * 2, i * 2 + 1, 0, 0));
    map.dump();
  }
  // Remove.
  for (int i = 1; i < 15; i++)
    map.erase(i * 4);

  for (int i = 60; i > 30; i--)
    map.insert(vma_t(i * 2, i * 2 + 1, 0, 0));

  // Remove until there are only two elements (58, 120).
  for (int i = 30; i < 60; i++)
    map.erase(i * 2);

  for (int i = 0; i < 14; i++)
    map.erase(i * 4 + 2);

  // Add a few more.
  for (int i = 10; i < 39; i += 7)
    map.insert(vma_t(i * 2, i * 2 + 1, 0, 0));

  // Add a little bit more.
  for (int i = 3; i < 171; i += 16)
    map.insert(vma_t(i * 2, i * 2 + 1, 0, 0));

  // Test iterators.
  for (const auto &[key, vma] : map)
    printk("%d: [%d - %d]\n", key, vma.begin, vma.end);

  // Test find.
  vma_t *vma = map.find(38);
  assert(vma);
  printk("%d - %d\n", vma->begin, vma->end);
  vma = map.find(8);
  assert(!vma);

  // Test gaps.
  int gaps[] = { 5, 10, 30, 60, 300 };
  for (auto gap : gaps)
    printk("gap of %d: %d\n", gap, map.find_gap(gap));

  printk("has overlap for [34, 199): %d\n", map.has_overlap(34, 199));
  os::vector<vma_t*> vec = map.find_overlap(34, 199);
  printk("overlap: ");
  for (auto v : vec)
    printk("%d ", v->begin);
  printk("\n");

  printk("has overlap for [77, 102): %d\n", map.has_overlap(77, 102));
}

void test() {
  test_btree();
}

#endif
