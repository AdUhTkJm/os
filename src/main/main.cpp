#include <stdint.h>
#include "../utils/libc.h"
#include "../mem/ptable.h"
#include "../mem/kalloc.h"
#include "../fs/initramfs.h"
#include "../fs/devfs.h"
#include "../fs/ext2.h"
#include "../fs/tmpfs.h"
#include "../fdt/fdt.h"
#include "../proc/elf.h"
#include "../proc/schedule.h"
#include "../driver/plic/plic.h"
#include "../driver/virtio/virtio.h"

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
[[noreturn, gnu::no_instrument_function]] __attribute__((section(".text.low")))
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

void idle() {
  for (;;)
    __asm__ volatile("wfi");
}

bool os::onboot = true;

void main_high() {
  // Set up page table.
  memset(__bss_begin, 0, __bss_end - __bss_begin);
  auto pt_root = (pte_t *) as_va((pa_t) 0x80201000);
  punmap((va_t) 0x80000000ul, MAP_1GB, pt_root);

  // Set up free list allocator.
  os::init_freelist_kalloc();

  // Set up (boot-time) kernel stack.
  boot_pcb.construct();
  RD(sp, boot_pcb->ksp);
  boot_pcb->pid = -1; // This is not a valid process.
  boot_pcb->pt_root = 0x80201000;
  scheduler.active = &boot_pcb;

  onboot = false;

  // Verify FDT.
  pa_t pfdt = *(pa_t *) as_va(0x80202010);
  int hart_id = *(uint64_t *) as_va(0x80202008);
  fdt::read(hart_id, pfdt);
  fdt::check();
  
  os::init_bitmap_kalloc();

  os::mount_initramfs(); // Constructs vfs.
  os::plic::init();
  os::mount_dev();
  os::mount_tmp();
  
  os::virtio::probe();

  // Register known, mountable fs'es.
  vfs->record("ext2", ext2_creator);
  vfs->record("tmp", tmp_creator);
  
  // Create an idle kernel process.
  scheduler.init();
  auto k_idle = make_kprocess(idle);
  scheduler.add(k_idle);

  // Start the init user process.
  file *init = vfs->open("/init", O_RDONLY);
  if (!init)
    panic("initramfs: cannot find /init");
  pcb_t *pcb = new (os::permanent) pcb_t;
  pcb->pid = nextpid();
  pcb->uid = pcb->gid = pcb->euid = pcb->suid = 0;
  if (load_elf(init, pcb) != 0)
    panic("load_elf: cannot load /init");
  os::init(pcb);
  scheduler.add(pcb);
  vfs->close(init);

  // Enable timer.
  sbi_set_timer(rv_rdtime() + 3000000);
  printk("Boot finished.\n");
  for (;;) ;
}
