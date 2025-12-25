#include "../mem/vma.h"
#include "../utils/stl/btree.h"

void test_btree() {
  os::vma::btree<4> map;

  // Insert.
  for (int i = 1; i < 30; i++)
    map.insert(i, os::vma::vma_t(i * 2, i * 2 + 1, 0, 0));

  // Remove.
  for (int i = 1; i < 15; i++)
    map.erase(i * 2);

  for (int i = 60; i > 30; i--)
    map.insert(i, os::vma::vma_t(i * 2, i * 2 + 1, 0, 0));

  // Remove until there are only two elements (29, 60).
  for (int i = 30; i < 60; i++)
    map.erase(i);

  for (int i = 0; i < 14; i++)
    map.erase(i * 2 + 1);

  // Add a few more.
  for (int i = 10; i < 39; i += 7)
    map.insert(i, os::vma::vma_t(i * 2, i * 2 + 1, 0, 0));

  // Add a little bit more.
  for (int i = 3; i < 171; i += 16)
    map.insert(i, os::vma::vma_t(i * 2, i * 2 + 1, 0, 0));
  
  // Test iterators.
  for (const auto &[key, vma] : map)
    printk("%d ", key);
  printk("\n");

  // Test find.
  os::vma::vma_t *vma = map.find(19);
  printk("%d - %d\n", vma->begin, vma->end);
  vma = map.find(8);
  printk("%p\n", vma);

  // Test gaps.
  printk("gap of 5: %d\n", map.find_gap(5));
}

void test() {
  test_btree();
}
