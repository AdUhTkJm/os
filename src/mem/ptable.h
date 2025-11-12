#ifndef PTABLE_H
#define PTABLE_H

#include <stdint.h>
#include <stddef.h>
#include "../utils/helper.h"

/**
We use the Sv39 paging scheme.

https://five-embeddev.com/riscv-priv-isa-manual/Priv-v1.12/supervisor.html#sec:sv39
*/

/* Page table entry. */
typedef uint64_t pte_t;
/* Virtual address. */
typedef uint64_t va_t;
/* Physical address. */
typedef uint64_t pa_t;

extern pte_t __pt_root[];

#define PTE_V (1ul << 0) /* Valid */
#define PTE_R (1ul << 1)
#define PTE_W (1ul << 2)
#define PTE_X (1ul << 3)
#define PTE_U (1ul << 4) /* User-space */
#define PTE_G (1ul << 5) /* Global */
#define PTE_A (1ul << 6) /* Access */
#define PTE_D (1ul << 7) /* Dirty */
#define PTE_RWX (PTE_R | PTE_W | PTE_X)
#define PTE_RX (PTE_R | PTE_X)
#define PTE_RW (PTE_R | PTE_W)
#define PTE_RSW(x) (((x) >> 8) & 0x3) /* Reserved for software */
#define PTE_FLAGS(x) (((x) >> 10) & 0x3ff)

#define PTE_PPN(x) (((x) >> 10) & ((1ul << 46) - 1))
#define PTE_PPN0(x) (((x) >> 10) & 0x1ff)
#define PTE_PPN1(x) (((x) >> 19) & 0x1ff)
#define PTE_PPN2(x) (((x) >> 28) & 0x3ffffff)
#define PPN_AS_PA(x) (((pa_t) x) << 12)

#define PTE_PPN_OFFSET 10
#define PTE_PPN0_OFFSET 10
#define PTE_PPN1_OFFSET 19
#define PTE_PPN2_OFFSET 28

/* Virtual address, level 0-2. */
#define VA_LVL2(x) (((x) >> 30) & 0x1ff)
#define VA_LVL1(x) (((x) >> 21) & 0x1ff)
#define VA_LVL0(x) (((x) >> 12) & 0x1ff)
#define VA_OFFSET(x) ((x) & 0xfff)

/* Physical address, level 0-2. */
#define PA_LVL2(x) (((x) >> 30) & 0x3ffffff)
#define PA_LVL1(x) (((x) >> 21) & 0x1ff)
#define PA_LVL0(x) (((x) >> 12) & 0x1ff)
#define PA_OFFSET(x) ((x) & 0xfff)

#define PA_AS_PPN(x) (((pa_t) x) >> 12)

#define SATP_MODE_SV39 (0x08ul << 60)

#define MAP_1GB 2
#define MAP_2MB 1
#define MAP_4KB 0

C void init_pagetable();
/* Gives a free 4KB physical frame. Note this expects a physical address. */
C void *pframe();
/* Frees a 4KB physical frame. Note this expects a physical address. */
C void pfree(void *p);

/*
Maps the given physical address into virtual address, with specified size.

Returns:
  - 0 for success.
  - 1 for invalid argument.
*/
C int pmap(pa_t pa, va_t va, int mode, unsigned flags);

/* Gives a virtually consecutive memory region of size `size`. */
C void *kalloc(size_t size);

#endif
