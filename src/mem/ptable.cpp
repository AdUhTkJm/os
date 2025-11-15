#include "ptable.h"
#include "kalloc.h"
#include "../utils/plic.h"
#include "../utils/libc.h"
#include "../fdt/fdt.h"

using os::operator""_mb;
using os::operator""_kb;

namespace {

bool is_leaf(pte_t pte) {
  return pte & PTE_RWX;
}

bool is_valid(pte_t pte) {
  return pte & PTE_V;
}

}

C void build_pagelist();

C int pmap(pa_t pa, va_t va, int mode, unsigned flags) {
  os::TLBRefreshGuard guard(va);
  // The maximum allowed for Sv39.
  if (va >= 0x7ffffffffful)
    return 1;

  // The flags must occupy bits 0.. PTE_PPN_OFFSET-1.
  // Hence this check.
  if (flags >= (1 << PTE_PPN_OFFSET))
    return 1;
  
  if (mode > 2 || mode < 0)
    return 1;

  // Allocate a 1GB node.
  if (mode == MAP_1GB) {
    pte_t pte = (PA_LVL2(pa) << PTE_PPN2_OFFSET) | flags;
    __pt_root[VA_LVL2(va)] = pte;
    return 0;
  }

  // Find the L1 page table.
  pte_t &pte_l2 = __pt_root[VA_LVL2(va)];
  pte_t *pt_l1 = nullptr;

  // When the page table is invalid, allocate a 4KB frame for L1 page table.
  // Note that in kernel, physical address and virtual address is identical.
  // So no worries about which we use.
  if (!is_valid(pte_l2)) {
    pt_l1 = (pte_t *) pframe();
    // Populate the L2 page table entry. It should record physical address
    // of the L1 page table.
    pte_l2 = (PA_AS_PPN(pt_l1) << PTE_PPN_OFFSET) | PTE_V;
  }

  // When the table is valid but is leaf, split it.
  if (is_leaf(pte_l2)) {
    // Still, create a L1 page table.
    pt_l1 = (pte_t *) pframe();

    // Fill the L1 table, such that the map doesn't change.
    auto orig_pa = PPN_AS_PA(PTE_PPN(pte_l2));
    for (int i = 0; i < 512; i++) {
      pt_l1[i] = (PA_LVL2(orig_pa) << PTE_PPN2_OFFSET)
        | (i << PTE_PPN1_OFFSET)
        | PTE_FLAGS(pte_l2);
    }

    // Record the location of L1 table in L2 table.
    pte_l2 = (PA_AS_PPN(pt_l1) << PTE_PPN_OFFSET) | PTE_V;
  }

  // If we haven't adjusted pt in previous parts, then this PTE is valid.
  // Hence we directly load from it.
  if (!pt_l1)
    pt_l1 = (pte_t *) PPN_AS_PA(PTE_PPN(pte_l2));

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
  pte_t *pt_l0 = nullptr;

  if (!is_valid(pte_l1)) {
    pt_l0 = (pte_t *) pframe();
    pte_l1 = (PA_AS_PPN(pt_l0) << PTE_PPN_OFFSET) | PTE_V;
  }

  if (is_leaf(pte_l1)) {
    pt_l0 = (pte_t *) pframe();
    auto orig_pa = PPN_AS_PA(PTE_PPN(pte_l1));
    for (int i = 0; i < 512; i++) {
      pt_l0[i] = (PA_LVL2(orig_pa) << PTE_PPN2_OFFSET)
        | (PA_LVL1(orig_pa) << PTE_PPN1_OFFSET)
        | (i << PTE_PPN0_OFFSET)
        | PTE_FLAGS(pte_l1);
    }
    pte_l1 = (PA_AS_PPN(pt_l0) << PTE_PPN_OFFSET) | PTE_V;
  }

  if (!pt_l0)
    pt_l0 = (pte_t *) PPN_AS_PA(PTE_PPN(pte_l1));

  // Populate the L0 page table entry.
  pte_t l0 = (PA_LVL2(pa) << PTE_PPN2_OFFSET)
    | (PA_LVL1(pa) << PTE_PPN1_OFFSET)
    | (PA_LVL0(pa) << PTE_PPN0_OFFSET)
    | flags;
  pt_l0[VA_LVL0(va)] = l0;

  return 0;
}

C unmap_ret_t punmap(va_t va, int mode) {
  if (mode > 2 || mode < 0)
    return { 0, UNMAP_SIZE_MISMATCH };

  os::TLBRefreshGuard guard(va);
  auto &pte_l2 = __pt_root[VA_LVL2(va)];

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

  auto *pt_l1 = (pte_t *) PPN_AS_PA(PTE_PPN(pte_l2));
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
  if (!is_leaf(pte_l1))
    return { 0, UNMAP_SIZE_MISMATCH };

  auto *pt_l0 = (pte_t *) PPN_AS_PA(PTE_PPN(pte_l1));
  pte_t &pte_l0 = pt_l0[VA_LVL0(va)];
  if (!is_valid(pte_l0))
    return { 0, UNMAP_NO_MAPPING };

  unmap_ret_t result = { PPN_AS_PA(PTE_PPN(pte_l0)), UNMAP_OK };
  pte_l0 = 0;
  return result;
}

C void init_pagetable() {
  // Initialize the free-list allocator for physical addresses.
  build_pagelist();

  // We give 128MB for kernel by identity-mapping. (64 * 2MB pages.)
  va_t kernel_va = 0x80000000ul;
  pa_t kernel_pa = 0x80000000ul;
  for (int i = 0; i < 64; i++)
    pmap(kernel_pa + i * 2_mb, kernel_va + i * 2_mb, MAP_2MB, PTE_RWX | PTE_G | PTE_V);

  // We also must map the PLIC and UART range.
  // PLIC spans 2 * 2MB pages. Still, we do an identity map. 
  for (int i = 0; i < 2; i++)
    pmap(PLIC_BASE + i * 2_mb, PLIC_BASE + i * 2_mb, MAP_2MB, PTE_RW | PTE_G | PTE_V | PTE_A | PTE_D);

  // Now set up the mapping of UART.
  // We will allocate a single 2MB page for it.
  pmap(UART_BASE, UART_BASE, MAP_2MB, PTE_RW | PTE_G | PTE_V | PTE_A | PTE_D);

  // Also set up the mapping for FDT.
  fdt_header_t *fdt = fdt_pos();
  for (unsigned i = 0; i < os::roundup<4_kb>(rev_endian(fdt->totalsize)); i += 4_kb)
    pmap((pa_t) fdt + i, (va_t) fdt + i, MAP_4KB, PTE_R | PTE_G | PTE_V);

  /* Move the root address into satp, and tell it we're using */
  /* virtual addresses now. */
  pa_t satp_val = SATP_MODE_SV39 | ((pa_t) __pt_root >> 12);
  __asm__ volatile(
    "csrw satp, %0\n"
    "sfence.vma zero, zero\n"
    :: "r"(satp_val) : "memory"
  );
}
