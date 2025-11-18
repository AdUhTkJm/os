#include <stdint.h>
#include "../utils/libc.h"
#include "../utils/plic.h"
#include "../mem/ptable.h"
#include "../fs/initramfs.h"
#include "../mem/kalloc.h"
#include "../fdt/fdt.h"

using namespace os;

static_assert(is_same_v<int64_t, long>);
static_assert(is_same_v<size_t, unsigned long>);

/*
Init layout:
.begin = 0x8020'0000
.text.low <= 4KB, cannot use stack
pt_root = 0x8020'1000
a0 = 0x8020'2008
a1 = 0x8020'2010
.text = 0x8020'3000, starting with _start_high
...
*/
[[noreturn]] __attribute__((section(".text.low")))
void kernel_main() {
  // Map 16GB memory.
  pte_t *root = (pte_t *) 0x80201000ul;
  for (unsigned long i = 0; i < 16; i++) {
    auto pa = i << 30, va = as_va(i << 30);
    pte_t pte = (PA_LVL2(pa) << PTE_PPN2_OFFSET) | PTE_RWX | PTE_G | PTE_V;
    root[VA_LVL2(va)] = pte;
  }
  // Temporarily identity-map.
  root[VA_LVL2(0x80000000ul)] = (PA_LVL2(0x80000000ul) << PTE_PPN2_OFFSET) | PTE_RWX | PTE_G | PTE_V;

  // Move the root address into satp, and tell it we're using virtual addresses now.
  pa_t satp_val = SATP_MODE_SV39 | ((pa_t) root >> 12);
  __asm__ volatile(
    "csrw satp, %0\n"
    "sfence.vma zero, zero\n"
    "jr %1\n"
    :: "r"(satp_val), "r"(as_va(0x80203000)) : "memory"
  );
  for (;;) ;
}

void init_timer();

void main_high() {
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  pt_root = (pte_t *) as_va((pa_t) 0x80201000);
  printk("Page table initialized.\n");
  os::init_freelist_kalloc();
  printk("Allocator initialized.\n");
  os::init_plic();
  printk("PLIC enabled.\n");

  // init_timer();
  // printk("Timer enabled.\n");

  auto *pfdt = (fdt::header *) as_va(0x80202010);
  int hart_id = *(uint64_t *) as_va(0x80202008);
  fdt::read(hart_id, pfdt);
  fdt::check();
  // printk("FDT checked at %p.\n", fdt::pos());
  // os::init_bitmap_kalloc();
  // printk("Bitmap allocator initialized.\n");
  // os::mount_initramfs();
  for (;;) ;
}
