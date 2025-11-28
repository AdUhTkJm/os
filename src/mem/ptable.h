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
typedef uintptr_t va_t;
/* Physical address. */
typedef uintptr_t pa_t;

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
#define PTE_COW (1ul << 8) /* Note that bits 8 and 9 are reserved for OS. */
#define PTE_FLAGS(x) ((x) & 0x3ff)

#define PTE_PPN(x) (((x) >> 10) & ((1ul << 46) - 1))
#define PTE_PPN0(x) (((x) >> 10) & 0x1ff)
#define PTE_PPN1(x) (((x) >> 19) & 0x1ff)
#define PTE_PPN2(x) (((x) >> 28) & 0x3ffffff)
#define PPN_AS_PA(x) ((pa_t) (x) << 12)

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

#define PAGE_SIZE 4096

// Map every address to higher-half.
#define KERNEL_OFFSET 0xffff'ffc0'0000'0000ul

#define PTE_TO_PA(x) (PPN_AS_PA(PTE_PPN(x)))
#define PTE_TO_VA(x) (as_va(PTE_TO_PA(x)))

namespace os {

// The active pt_root, virtual address.
extern pte_t *pt_root;
// The kernel's pt_root.
extern pte_t *kernel_pt_root;

/*
Maps the given physical address into virtual address, with specified size.

Returns:
  - 0 for success.
  - 1 for invalid argument.
*/
int pmap(pa_t pa, va_t va, int mode, unsigned flags, pte_t *root = pt_root);
inline int pmap(pa_t pa, const void *va, int mode, unsigned flags, pte_t *root = pt_root) {
  return pmap(pa, (va_t) va, mode, flags, root);
}

typedef enum {
  UNMAP_OK,
  UNMAP_NO_MAPPING,
  UNMAP_SIZE_MISMATCH,
} unmap_status_t;

typedef struct {
  pa_t pa;
  unmap_status_t status;
} unmap_ret_t;

/*
Unmaps the given virtual address. If it is mapped to some physical address,
then the address must span the specified size. Otherwise, the behaviour is
undefined.
Returns the physical address that this table is previously mapped to. If
there is no such address, returns 0.
*/
unmap_ret_t punmap(va_t va, int mode, pte_t *root = pt_root);
inline unmap_ret_t punmap(const void *va, int mode, pte_t *root = pt_root) { return punmap((va_t) va, mode, root); }

// Sv39 requires that the virtual address is sign-extended on bit 38 (highest bit).
inline constexpr va_t sext(va_t x) {
  return (((x >> 38) & 1) ? x | 0xffff'ff80'0000'0000 : x);
}

// This is a direct mapping from PA to VA, that works on the whole kernel.
// The to_pa() function actually walks the table.
// This must be always_inline, because it is called both before and after
// setting up the page table.
[[gnu::always_inline]] inline constexpr va_t as_va(pa_t pa) {
  return pa + KERNEL_OFFSET;
}

pa_t to_pa(va_t va, pte_t *root = pt_root);
inline pa_t to_pa(const void *va, pte_t *root = pt_root) { return to_pa((va_t) va, root); }

// Returns -1 when the PTE is not valid.
int pte_flags(va_t va, pte_t *root = pt_root);
inline int pte_flags(const void *va, pte_t *root = pt_root) { return pte_flags((va_t) va, root); }

/* Gives a virtually consecutive memory region of size `size`. */
void *kalloc(size_t size);

namespace pt {

inline bool is_leaf(pte_t pte) {
  return pte & PTE_RWX;
}

inline bool is_valid(pte_t pte) {
  return pte & PTE_V;
}

// This will only provide valid entries to the function `f`.
template<class T> requires requires(T f, pte_t &t) { f(t); }
void walk(pte_t *root, T f) {
  for (unsigned i = 0; i < 512; i++) {
    if (!is_valid(root[i]))
      continue;
    
    f(root[i]);
    if (!is_leaf(root[i]))
      walk((pte_t *) PTE_TO_VA(root[i]), f);
  }
}

pa_t copy(pte_t *pt);
void free(pa_t pt);

}

struct TLBRefreshGuard {
  va_t flushed;

  TLBRefreshGuard(): flushed(0) {}
  explicit TLBRefreshGuard(va_t va): flushed(va) {}
  ~TLBRefreshGuard() {
    __asm__ volatile(
      "sfence.vma %0, zero\n"
      :: "r"(flushed) : "memory"
    );
  }
};

struct EnableAccessToUserMemory {
  reg_t sstatus;
  
  EnableAccessToUserMemory() {
    __asm__ volatile("csrr %0, sstatus" : "=r"(sstatus));
    __asm__ volatile("csrs sstatus, %0" :: "r"(1 << 18));
  }
  ~EnableAccessToUserMemory() {
    __asm__ volatile("csrw sstatus, %0" :: "r"(sstatus));
  }
};

// Read and write to physical memory.

template<class T> requires os::is_integral_v<T>
T mmrd(pa_t p) {
  return *(volatile T*) os::as_va(p);
}

template<class T> requires os::is_integral_v<T>
void mmwr(pa_t p, T value) {
  *(volatile T*) os::as_va(p) = (T) value;
}

template<class T> requires __is_enum(T)
void mmwr(pa_t p, T value) {
  using U = underlying_type<T>::type;
  *(volatile U*) os::as_va(p) = (U) value;
}

void copy_to_user(void *usr, void *ker, size_t len);

}
#endif
