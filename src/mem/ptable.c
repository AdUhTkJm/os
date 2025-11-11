#include "ptable.h"
#include "../utils/plic.h"
#include "../utils/libc.h"

void build_pagelist();

void init_pagetable() {
  build_pagelist();

  /* We directly allocate an 1GB leaf entry by identity-mapping. */
  /* This is for bootstrapping only; the entry will be rewritten later. */
  va_t kernel_va = 0x80000000ul;
  pa_t kernel_pa = 0x80000000ul;
  pte_t pte = PA_LVL2(kernel_pa) << PTE_PPN2_OFFSET;
  pte |= PTE_R | PTE_W | PTE_X | PTE_G | PTE_V;
  __pt_root[VA_LVL2(kernel_va)] = pte;

  /* We also must map the PLIC and UART range. */
  /* PLIC spans 2*2MB pages. Still, we do an identity map. */
  va_t plic_va = PLIC_BASE;
  pa_t plic_pa = PLIC_BASE;

  /* Set up page table in level 2. */
  /* The content of this PTE should point to the next level of page table, */
  /* which is pt_plic. */
  pte_t *pt_plic = (pte_t *) pframe();
  pte_t pte_l2_plic = PA_AS_PPN(pt_plic) << PTE_PPN_OFFSET;
  pte_l2_plic |= PTE_V;
  __pt_root[VA_LVL2(plic_va)] = pte_l2_plic;

  /* Set up page table in level 1. */
  for (int i = 0; i < 2; i++) {
    pa_t pa = plic_pa + i * 0x200000;
    pa_t va = plic_va + i * 0x200000;
    pte_t pte_l1_plic = PA_LVL2((pa_t) pa) << PTE_PPN2_OFFSET;
    pte_l1_plic |= PA_LVL1((pa_t) pa) << PTE_PPN1_OFFSET;
    pte_l1_plic |= PTE_R | PTE_W | PTE_G | PTE_V | PTE_A | PTE_D;

    pt_plic[VA_LVL1(va)] = pte_l1_plic;
  }

  /* Now set up the mapping of UART. */
  /* We will allocate a single 2MB page for it. */
  va_t uart_va = UART_BASE;
  pa_t uart_pa = UART_BASE;

  pte_t *pt_uart = (pte_t *) pframe();
  pte_t pte_l2_uart = PA_AS_PPN(pt_uart) << PTE_PPN_OFFSET;
  pte_l2_uart |= PTE_V;
  __pt_root[VA_LVL2(uart_va)] = pte_l2_uart;

  pte_t pte_l1_uart = PA_LVL2((pa_t) uart_pa) << PTE_PPN2_OFFSET;
  pte_l1_uart |= PA_LVL1((pa_t) uart_pa) << PTE_PPN1_OFFSET;
  pte_l1_uart |= PTE_R | PTE_W | PTE_G | PTE_V | PTE_A | PTE_D;
  pt_uart[VA_LVL1(uart_va)] = pte_l1_uart;

  /* Move the root address into satp, and tell it we're using */
  /* virtual addresses now. */
  pa_t satp_val = SATP_MODE_SV39 | ((pa_t) __pt_root >> 12);
  __asm__ volatile(
    "csrw satp, %0\n"
    "sfence.vma zero, zero\n"
    :: "r"(satp_val) : "memory"
  );
}
