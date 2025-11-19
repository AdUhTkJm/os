#include "ptable.h"
#include "kalloc.h"
#include "../utils/plic.h"
#include "../utils/libc.h"
#include "../fdt/fdt.h"

namespace {

bool is_leaf(pte_t pte) {
  return pte & PTE_RWX;
}

bool is_valid(pte_t pte) {
  return pte & PTE_V;
}

}

void build_page_list();
void main_high();

namespace os {

pte_t *pt_root, *kernel_pt_root;

int pmap(pa_t pa, va_t va, int mode, unsigned flags) {
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
    pt_root[VA_LVL2(va)] = pte;
    return 0;
  }

  // Find the L1 page table.
  pte_t &pte_l2 = pt_root[VA_LVL2(va)];
  pa_t pa_pt_l1 = 0;

  // When the page table is invalid, allocate a 4KB frame for L1 page table.
  // Note that in kernel, physical address and virtual address is identical.
  // So no worries about which we use.
  if (!is_valid(pte_l2)) {
    pa_pt_l1 = pframe();
    // Populate the L2 page table entry. It should record physical address
    // of the L1 page table.
    pte_l2 = (PA_AS_PPN(pa_pt_l1) << PTE_PPN_OFFSET) | PTE_V;
  }

  // When the table is valid but is leaf, split it.
  if (is_leaf(pte_l2)) {
    // Still, create a L1 page table.
    pa_pt_l1 = pframe();

    // Fill the L1 table, such that the map doesn't change.
    auto *pt_l1 = (pte_t *) as_va(pa_pt_l1);
    auto orig_pa = PPN_AS_PA(PTE_PPN(pte_l2));
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
    pa_pt_l1 = PPN_AS_PA(PTE_PPN(pte_l2));
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
    pa_pt_l0 = pframe();
    pte_l1 = (PA_AS_PPN(pa_pt_l0) << PTE_PPN_OFFSET) | PTE_V;
  }

  if (is_leaf(pte_l1)) {
    pa_pt_l0 = pframe();
    auto orig_pa = PPN_AS_PA(PTE_PPN(pte_l1));
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
    pa_pt_l0 = PPN_AS_PA(PTE_PPN(pte_l1));
  auto *pt_l0 = (pte_t *) as_va(pa_pt_l0);

  // Populate the L0 page table entry.
  pte_t l0 = (PA_LVL2(pa) << PTE_PPN2_OFFSET)
    | (PA_LVL1(pa) << PTE_PPN1_OFFSET)
    | (PA_LVL0(pa) << PTE_PPN0_OFFSET)
    | flags;
  pt_l0[VA_LVL0(va)] = l0;

  return 0;
}

unmap_ret_t punmap(va_t va, int mode) {
  if (mode > 2 || mode < 0)
    return { 0, UNMAP_SIZE_MISMATCH };

  os::TLBRefreshGuard guard(va);
  auto &pte_l2 = pt_root[VA_LVL2(va)];

  if (!is_valid(pte_l2))
    return { 0, UNMAP_NO_MAPPING };

  if (mode == MAP_1GB) {
    if (!is_leaf(pte_l2))
      return { 0, UNMAP_SIZE_MISMATCH };

    unmap_ret_t result = { PPN_AS_PA(PTE_PPN(pte_l2)), UNMAP_OK };
    pte_l2 = 0;
    return result;
  }
  if (is_leaf(pte_l2))
    return { 0, UNMAP_SIZE_MISMATCH };

  auto *pt_l1 = (pte_t *) as_va(PPN_AS_PA(PTE_PPN(pte_l2)));
  pte_t &pte_l1 = pt_l1[VA_LVL1(va)];

  if (!is_valid(pte_l1))
    return { 0, UNMAP_NO_MAPPING };

  if (mode == MAP_2MB) {
    if (!is_leaf(pte_l1))
      return { 0, UNMAP_SIZE_MISMATCH };

    unmap_ret_t result = { PPN_AS_PA(PTE_PPN(pte_l1)), UNMAP_OK };
    pte_l1 = 0;
    return result;
  }
  if (is_leaf(pte_l1))
    return { 0, UNMAP_SIZE_MISMATCH };

  auto *pt_l0 = (pte_t *) as_va(PPN_AS_PA(PTE_PPN(pte_l1)));
  pte_t &pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return { 0, UNMAP_NO_MAPPING };

  unmap_ret_t result = { PPN_AS_PA(PTE_PPN(pte_l0)), UNMAP_OK };
  pte_l0 = 0;
  return result;
}

pa_t to_pa(va_t va) {
  pte_t pte_l2 = pt_root[VA_LVL2(va)];
  if (!is_valid(pte_l2))
    return -1ul;
  if (is_leaf(pte_l2))
    return PPN_AS_PA(PTE_PPN(pte_l2))
      + (VA_LVL1(va) << 21)
      + (VA_LVL0(va) << 12)
      + VA_OFFSET(va);

  pte_t *pt_l1 = (pte_t *) as_va(PPN_AS_PA(PTE_PPN(pte_l2)));
  pte_t pte_l1 = pt_l1[VA_LVL1(va)];

  if (!is_valid(pte_l1))
    return -1ul;
  if (is_leaf(pte_l1))
    return PPN_AS_PA(PTE_PPN(pte_l1))
      + (VA_LVL0(va) << 12)
      + VA_OFFSET(va);

  pte_t *pt_l0 = (pte_t *) as_va(PPN_AS_PA(PTE_PPN(pte_l1)));
  pte_t pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return -1ul;

  return PPN_AS_PA(PTE_PPN(pte_l0)) + VA_OFFSET(va);
}

}
