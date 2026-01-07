#include "../mem/kalloc.h"
#include "../mem/shm.h"
#include "../fs/initramfs.h"
#include "../fs/devfs.h"
#include "../fs/ext.h"
#include "../fs/tmpfs.h"
#include "../fs/procfs.h"
#include "../fdt/fdt.h"
#include "../proc/elf.h"
#include "../proc/schedule.h"
#include "../driver/plic/plic.h"
#include "../driver/virtio/virtio.h"
#include "../lock/futex.h"

using namespace os;

static_assert(is_same_v<int64_t, long>);
static_assert(is_same_v<size_t, unsigned long>);

#ifdef UNIT_TEST
void test();
#endif

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
[[noreturn, gnu::no_instrument_function, gnu::section(".text.low")]]
void kernel_main() {
#ifdef RV
  // Map 16GB memory.
  pte_t *root = (pte_t *) 0x80201000ul;
  for (unsigned long i = 0; i < 16; i++) {
    auto pa = i << 30, va = as_va(i << 30);
    pte_t pte = (PA_LVL2(pa) << PTE_PPN2_OFFSET) | PTE_RWX | PTE_G | PTE_V;
    root[VA_LVL2(va)] = pte;
  }
  // Temporarily identity-map.
  root[VA_LVL2(0x8000'0000ul)] = (PA_LVL2(0x8000'0000ul) << PTE_PPN2_OFFSET) | PTE_RWX | PTE_G | PTE_V;

  // Move the root address into satp, and tell it we're using virtual addresses now.
  pa_t satp_val = SATP_MODE_SV39 | ((pa_t) root >> 12);
  __asm__ volatile(
    "csrw satp, %0\n"
    "sfence.vma zero, zero\n"
    "jr %1\n"
    :: "r"(satp_val), "r"(as_va(0x80203000)) : "memory"
  );
  __builtin_unreachable();
#endif

#ifdef LA
#define MAP_2G(pa, va, index) \
  CSRW(tlbehi, ((va) & (~((1ul << 31) - 1)))); \
  CSRW(tlblo0, ((pa) & (~((1UL << TLBLO_PPN_SHIFT) - 1))) | TLBLO_V | TLBLO_D | TLBLO_G); /* PLV0 == 0, so isn't or'ed here. */ \
  CSRW(tlblo1, ((pa + (1 << 30)) & (~((1UL << TLBLO_PPN_SHIFT) - 1))) | TLBLO_V | TLBLO_D | TLBLO_G); \
  CSRW(tlbidx, (30ul << 24) | index); \
  __asm__ volatile("tlbfill");

  // First invalidate all TLBs in case there are any remaining.
  __asm__ volatile("invtlb 0, $zero, $zero");
  // Disable hardware walkers.
  CSRW(pwcl, 0);
  // Disable direct memory.
  CSRW(dmw0, 0); CSRW(dmw1, 0); CSRW(dmw2, 0); CSRW(dmw3, 0);

  // Map the first 16 GB memory as is done in RISC-V.
  for (unsigned long i = 0; i < 8; i++) {
    MAP_2G((i << 31), as_va(i << 31), i);
  }
  MAP_2G(0, 0, 16);
#undef MAP_1G
  __asm__ volatile("ibar 0");
  CSRW(asid, 0);
  CSRW(tlbrentry, 0x8);

  unsigned long crmd;
  CSRR(crmd, crmd);
  crmd &= ~CRMD_DA;
  crmd |= CRMD_PG;
  CSRW(crmd, crmd);
  __asm__ volatile("jirl $zero, %0, 0\n" :: "r"(as_va(0x203000)));
  __builtin_unreachable();
#endif
}

// I tried calling `make_zeroes` on idle to make page faults faster.
// But it turned out that this won't work well - it becomes even slower.
#ifdef RV
void idle() {
  for (;;) {
    __asm__ volatile("wfi");
  }
}
#endif

#ifdef LA
void idle() {
  for (;;)
    __asm__ volatile("idle 0");
}

// The Loongarch boot interface.
struct boot_tag {
  unsigned magic;
  unsigned len;
  // Data follows...
};

struct efi {

};

pa_t find_dtb(pa_t start) {
  auto tag = (boot_tag *) as_va(start);
  for (int i = 0; i < 100; i++) {
    if (tag->magic == 0x66424454) // fDTB
      return *(unsigned long *)((char *) tag + 8);
    
    tag = (boot_tag *) ((char *) tag + tag->len);
  }
  return 0;
}
#endif

bool os::onboot = true;
// Used in interrupt.h. Tick length in nanoseconds.
int clock_period;
// Nanoseconds since Unix epoch (1970.1.1)
size_t realtime;

void get_tick() {
  void *p = fdt::query("/cpus", "timebase-frequency");
  clock_period = 1'000'000'000 / to_big_endian(*(unsigned *) p);
}

void get_real_time() {
  char *p = (char *) fdt::pfdt + to_big_endian(fdt::pfdt->off_dt_struct);
  fdt::walk(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    (void) len; (void) property;
    if (strncmp("/soc/rtc@", cdev, 9) != 0)
      return WalkResult::Continue;
    
    if (strcmp("compatible", cprop) == 0) {
      if (strcmp((char *) property, "google,goldfish-rtc") != 0) {
        printk("rtc: unknown compatible value %s\n", property);
        panic("rtc: unknown property");
      }
      return WalkResult::Continue;
    }

    if (strcmp("reg", cprop) == 0) {
      // For meaning, see:
      //   https://android.googlesource.com/platform/external/qemu/%2B/master/docs/GOLDFISH-VIRTUAL-HARDWARE.TXT
      // This device is introduced on line 213.
      auto base = pa_t((fdt::detail::read_int(property) * 1ul << 32) + fdt::detail::read_int((char*) property + 4));
      realtime = mmrd<unsigned>(base);
      realtime |= size_t(mmrd<unsigned>(base + 4)) << 32;
      srand(realtime);
      return WalkResult::Interrupt;
    }
    
    return WalkResult::Continue;
  });
}

void main_high() {
  // Set up page table.
  // Don't directly subtract the arrays: that is UB.
  memset(__bss_begin, 0, (pa_t) __bss_end - (pa_t) __bss_begin);
  auto pt_root = (pte_t *) as_va((pa_t) __kernel_pt_root);
#ifdef RV
  punmap((va_t) 0x80000000ul, MAP_1GB, pt_root);
#endif

#ifdef LA
  // In Loongarch, we haven't actually populated the software page table.
  // So we do it manually here.
  for (unsigned long i = 0; i < 16; i++) {
    auto pa = i << 30, va = as_va(i << 30);
    pte_t pte = (PA_LVL2(pa) << PTE_PPN2_OFFSET) | PTE_RWX | PTE_G | PTE_V;
    pt_root[VA_LVL2(va)] = pte;
  }
#endif

  // Clear the half of user-space to make sure there won't be bad entries when copying.
  memset(pt_root, 0, PAGE_SIZE / 2);

  // Set up free list allocator.
  os::init_freelist_kalloc();

  // Verify FDT.
#ifdef RV
  pa_t pfdt = *(pa_t *) as_va(0x80202010);
  int hart_id = *(uint64_t *) as_va(0x80202008);
#endif

#ifdef LA
  int hart_id = 0;
  // This points to the EFI system table.
  printk("%p\n", mmrd<unsigned>(0x10200));
  pa_t pfdt = 0;
  if (!pfdt)
    panic("no dtb found");

  printk("dtb = %p\n", pfdt);
#endif
  fdt::read(hart_id, pfdt);
  fdt::check();
  
  os::init_bitmap_kalloc();

  // Initialize various global variables.
  os::vma::init();
  os::siginit();
  os::shm::init();

  // Set up (boot-time) kernel stack.
  boot_pcb.construct();
  boot_tcb.construct();
  RD(sp, boot_tcb->ksp);
  boot_tcb->status = Running;
  boot_pcb->pid = -1; // This is not a valid process.
  boot_pcb->pt_root = 0x80201000;
  boot_tcb->pcb = boot_pcb.get();
  boot_pcb->threads.push_back(boot_tcb.get());
  scheduler.active = boot_tcb.get();

  onboot = false;

  // Initialize global vfs structure.
  vfs::init();
  // We need to get real time in order to make sure metadata is working.
  get_tick();
  get_real_time();

  os::mount_initramfs();
  os::plic::init();
  os::mount_dev();
  os::mount_tmp();

  // At this time we're already prepared enough to execute unit test.
#ifdef UNIT_TEST
  test();
  sbi_system_reset();
#endif
  
  os::virtio::probe();

  // Register known, mountable fs'es.
  vfs::record("ext2", ext_creator);
  vfs::record("tmpfs", tmp_creator);
  vfs::record("procfs", procfs_creator);
  
  // Create an idle kernel process.
  pidmap.construct();
  scheduler.init();
  auto k_idle = make_kprocess(idle);
  scheduler.add(k_idle);

  // Start the init user process.
  pcb_t *pcb = new (os::permanent) pcb_t;
  tcb_t *tcb = new (os::permanent) tcb_t;
  tcb->pcb = pcb;
  pcb->threads.push_back(tcb);
  pcb->parent = nullptr;
  pcb->vma = new vma::addrspace;
  pcb->vma->ref();

  pcb->vfs = boot_pcb->vfs;
  pcb->vfs->ref();
  file *init = pcb->vfs->open("/init", O_RDONLY);
  if (!init)
    panic("initramfs: cannot find /init");
  
  // Initialize the basic PCB structure.
  pcb->pid = pcb->pgid = pcb->sid = nextpid();
  tcb->tid = pcb->pid;
  (*pidmap)[pcb->pid] = pcb;
  pcb->uid = pcb->euid = pcb->suid = 0;
  pcb->gid = pcb->egid = pcb->sgid = 0;
  pcb->pwd = *pcb->vfs->lookup("/");
  pcb->ftbl = new process_file_table;
  pcb->ftbl->ref();
  pcb->execpath = "/init";

  // Gives one physical page for the root page table and the kernel stack.
  pcb->pt_root = pframe();
  tcb->ksp = (va_t) vmalloc<16>(kstack_size) + kstack_size - sizeof(trapframe);
  ((trapframe *) tcb->ksp)->sscratch = 0xf'f000'0000;

  // Copy the kernel's level 2 root table.
  // We only need to shallow copy.
  memcpy((void *) as_va(pcb->pt_root), (void*) as_va(__kernel_pt_root), PAGE_SIZE);

  // Open stdin, stdout and stderr.
  // Note they are different files, but point to the same place.
  auto console = pcb->vfs->lookup("/dev/console");
  if (!console)
    panic("no console!");
  pcb->ftbl->allocate(new file(*console, O_RDONLY), 0); // stdin
  pcb->ftbl->allocate(new file(*console, O_WRONLY), 1); // stdout
  pcb->ftbl->allocate(new file(*console, O_WRONLY), 2); // stderr

  if (!load_elf(init, tcb).valid())
    panic("load_elf: cannot load /init");
  scheduler.add(tcb);
  pcb->vfs->close(init);

  // Create a DHCP daemon that updates IP address.
  // We must create it after `init`, so that `init` will have PID 1.
  ip::routes.construct();
  // TODO: looks like there's some problem with DHCP. Looks like race condition;
  // perhaps only do it after `init`?
  auto dhcp = make_kprocess(dhcp::daemon);
  scheduler.add(dhcp);

  // Enable futexes.
  futexes.construct();

  // Enable timer.
  sbi_set_timer(rdtime() + 10_ms / clock_period);
  printk("Boot finished.\n");
  for (;;) ;
}
