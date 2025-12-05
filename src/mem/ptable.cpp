#include "ptable.h"
#include "kalloc.h"
#include "vma.h"
#include "../driver/plic/plic.h"
#include "../utils/libc.h"
#include "../fdt/fdt.h"
#include "../proc/schedule.h"

void build_page_list();
void main_high();

namespace {
  using namespace os;
  using namespace os::pt;

  pa_t copy_impl(pte_t *pt, int lvl) {
    pa_t root = pframe_zeroed();
    unsigned end = 512;
    if (lvl == 2) {
      // Only recurse into the bottom 256 entries.
      // The upper half are OS pages and are shared across all processes.
      memcpy((pte_t*) as_va(root) + 256, pt + 256, PAGE_SIZE / 2);
      end = 256;
    }

    auto va = (pte_t *) as_va(root);
    for (unsigned i = 0; i < end; i++) {
      if (!is_valid(pt[i]))
        continue;
      if (is_leaf(pt[i])) {
        va[i] = pt[i];
        if (pt[i] & PTE_U)
          pincref(PTE_TO_PA(pt[i]));
        continue;
      }
      auto pa = copy_impl((pte_t*) PTE_TO_VA(pt[i]), lvl - 1);
      va[i] = (PA_AS_PPN(pa) << PTE_PPN_OFFSET) | PTE_FLAGS(pt[i]);
    }
    return root;
  }

  void free_impl(pte_t *pt, int lvl) {
    unsigned end = lvl == 2 ? 256 : 512;
    // No more child nodes to free.
    if (lvl == 0)
      return;
    for (unsigned i = 0; i < end; i++) {
      if (is_leaf(pt[i]) || !is_valid(pt[i]))
        continue;
      free_impl((pte_t *) PTE_TO_VA(pt[i]), lvl - 1);
      pfree(PTE_TO_PA(pt[i]));
    }
  }
}

namespace os {

using namespace pt;

int pmap(pa_t pa, va_t va, int mode, unsigned flags, pte_t *root) {
  os::TLBRefreshGuard guard(va);

  // The flags must occupy bits 0.. PTE_PPN_OFFSET-1.
  // Hence this check.
  if (flags >= (1 << PTE_PPN_OFFSET))
    return 1;
  
  if (mode > 2 || mode < 0)
    return 1;

  // Allocate a 1GB node.
  if (mode == MAP_1GB) {
    pte_t pte = (PA_LVL2(pa) << PTE_PPN2_OFFSET) | flags;
    root[VA_LVL2(va)] = pte;
    return 0;
  }

  // Find the L1 page table.
  pte_t &pte_l2 = root[VA_LVL2(va)];
  pa_t pa_pt_l1 = 0;

  // When the page table is invalid, allocate a 4KB frame for L1 page table.
  // Note that in kernel, physical address and virtual address is identical.
  // So no worries about which we use.
  if (!is_valid(pte_l2)) {
    pa_pt_l1 = pframe_zeroed();
    // Populate the L2 page table entry. It should record physical address
    // of the L1 page table.
    pte_l2 = (PA_AS_PPN(pa_pt_l1) << PTE_PPN_OFFSET) | PTE_V;
  }

  // When the table is valid but is leaf, split it.
  if (is_leaf(pte_l2)) {
    // Still, create a L1 page table.
    pa_pt_l1 = pframe_zeroed();

    // Fill the L1 table, such that the map doesn't change.
    auto *pt_l1 = (pte_t *) as_va(pa_pt_l1);
    auto orig_pa = PTE_TO_PA(pte_l2);
    for (int i = 0; i < 512; i++) {
      pt_l1[i] = (PA_LVL2(orig_pa) << PTE_PPN2_OFFSET)
        | (i << PTE_PPN1_OFFSET)
        | PTE_FLAGS(pte_l2);
    }

    // Record the location of L1 table in L2 table.
    pte_l2 = (PA_AS_PPN(pa_pt_l1) << PTE_PPN_OFFSET) | PTE_V;
  }

  // If we haven't adjusted pt in previous parts, then this PTE is valid.
  // Hence we directly load from it.
  if (!pa_pt_l1)
    pa_pt_l1 = PTE_TO_PA(pte_l2);
  auto *pt_l1 = (pte_t *) as_va(pa_pt_l1);

  // Allocate a 2MB node.
  if (mode == MAP_2MB) {
    // Populate the L1 page table entry.
    pte_t l1 = (PA_LVL2(pa) << PTE_PPN2_OFFSET)
      | (PA_LVL1(pa) << PTE_PPN1_OFFSET)
      | flags;
    pt_l1[VA_LVL1(va)] = l1;
    return 0;
  }

  // Allocate a 4KB node.
  // Similar to above, we try to find the L0 table.
  pte_t &pte_l1 = pt_l1[VA_LVL1(va)];
  pa_t pa_pt_l0 = 0;

  if (!is_valid(pte_l1)) {
    pa_pt_l0 = pframe_zeroed();
    pte_l1 = (PA_AS_PPN(pa_pt_l0) << PTE_PPN_OFFSET) | PTE_V;
  }

  if (is_leaf(pte_l1)) {
    pa_pt_l0 = pframe_zeroed();
    auto orig_pa = PTE_TO_PA(pte_l1);
    auto *pt_l0 = (pte_t *) as_va(pa_pt_l0);
    for (int i = 0; i < 512; i++) {
      pt_l0[i] = (PA_LVL2(orig_pa) << PTE_PPN2_OFFSET)
        | (PA_LVL1(orig_pa) << PTE_PPN1_OFFSET)
        | (i << PTE_PPN0_OFFSET)
        | PTE_FLAGS(pte_l1);
    }
    pte_l1 = (PA_AS_PPN(pa_pt_l0) << PTE_PPN_OFFSET) | PTE_V;
  }

  if (!pa_pt_l0)
    pa_pt_l0 = PTE_TO_PA(pte_l1);
  auto *pt_l0 = (pte_t *) as_va(pa_pt_l0);

  // Populate the L0 page table entry.
  pte_t l0 = (PA_LVL2(pa) << PTE_PPN2_OFFSET)
    | (PA_LVL1(pa) << PTE_PPN1_OFFSET)
    | (PA_LVL0(pa) << PTE_PPN0_OFFSET)
    | flags;
  pt_l0[VA_LVL0(va)] = l0;

  return 0;
}

int pmap(pa_t pa, va_t va, int mode, unsigned flags) {
  return pmap(pa, va, mode, flags, pt_root());
}

expected<pa_t> punmap(va_t va, int mode, pte_t *root) {
  if (mode > 2 || mode < 0)
    return -BADSIZE;

  os::TLBRefreshGuard guard(va);
  auto &pte_l2 = root[VA_LVL2(va)];

  if (!is_valid(pte_l2))
    return -MAPLESS;

  if (mode == MAP_1GB) {
    if (!is_leaf(pte_l2))
      return -BADSIZE;

    auto result = PTE_TO_PA(pte_l2);
    pte_l2 = 0;
    return result;
  }
  if (is_leaf(pte_l2))
    return -BADSIZE;

  auto *pt_l1 = (pte_t *) PTE_TO_VA(pte_l2);
  pte_t &pte_l1 = pt_l1[VA_LVL1(va)];

  if (!is_valid(pte_l1))
    return -MAPLESS;

  if (mode == MAP_2MB) {
    if (!is_leaf(pte_l1))
      return -BADSIZE;

    auto result = PTE_TO_PA(pte_l1);
    pte_l1 = 0;
    return result;
  }
  if (is_leaf(pte_l1))
    return -BADSIZE;

  auto *pt_l0 = (pte_t *) PTE_TO_VA(pte_l1);
  pte_t &pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return -MAPLESS;

  auto result = PTE_TO_PA(pte_l0);
  pte_l0 = 0;
  return result;
}

expected<pa_t> punmap(va_t va, int mode) {
  return punmap(va, mode, pt_root());
}

pte_t *pt_root() {
  [[unlikely]] if (onboot)
    return (pte_t *) as_va(__kernel_pt_root);

  return (pte_t *) as_va(scheduler.active->pt_root);
}

pa_t to_pa(va_t va, const pte_t *root) {
  pte_t pte_l2 = root[VA_LVL2(va)];
  if (!is_valid(pte_l2))
    return -1ul;
  if (is_leaf(pte_l2))
    return PTE_TO_PA(pte_l2)
      + (VA_LVL1(va) << 21)
      + (VA_LVL0(va) << 12)
      + VA_OFFSET(va);

  pte_t *pt_l1 = (pte_t *) PTE_TO_VA(pte_l2);
  pte_t pte_l1 = pt_l1[VA_LVL1(va)];

  if (!is_valid(pte_l1))
    return -1ul;
  if (is_leaf(pte_l1))
    return PTE_TO_PA(pte_l1)
      + (VA_LVL0(va) << 12)
      + VA_OFFSET(va);

  pte_t *pt_l0 = (pte_t *) PTE_TO_VA(pte_l1);
  pte_t pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return -1ul;

  return PTE_TO_PA(pte_l0) + VA_OFFSET(va);
}

pa_t to_pa(va_t va) {
  return to_pa(va, pt_root());
}

int pte_flags(va_t va, const pte_t *root) {
  pte_t pte_l2 = root[VA_LVL2(va)];
  if (!is_valid(pte_l2))
    return -1;
  if (is_leaf(pte_l2))
    return PTE_FLAGS(pte_l2);

  pte_t *pt_l1 = (pte_t *) PTE_TO_VA(pte_l2);
  pte_t pte_l1 = pt_l1[VA_LVL1(va)];

  if (!is_valid(pte_l1))
    return -1;
  if (is_leaf(pte_l1))
    return PTE_FLAGS(pte_l1);

  pte_t *pt_l0 = (pte_t *) PTE_TO_VA(pte_l1);
  pte_t pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return -1;

  return PTE_FLAGS(pte_l0);
}

int pte_flags(va_t va) {
  return pte_flags(va, pt_root());
}

namespace pt {

pa_t copy(pte_t *root) {
  return copy_impl(root, 2);
}

void free(pa_t root) {
  free_impl((pte_t*) as_va(root), 2);
  pfree(root);
}

}

}
