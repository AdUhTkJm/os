#ifndef HELPER_H
#define HELPER_H

#include "sbi.h"
#include "errorcode.h"

#if !defined(__clang__) && !defined(__GNUC__)
#error This OS kernel must be compiled with clang or GNUC.
#endif

#ifdef __cplusplus
#  define C extern "C" [[gnu::no_instrument_function]] 
// #include must be together for the build script to detect dependencies.
#include "helper.hpp"
#else
#  define C
#endif

/* I always don't remember the exact form of test macros, so redefine them here. */
/* This also helps VSCode to highlight - just change position of IN_VSCODE. */
#if defined(__riscv) || IN_VSCODE
#  define RV
#elif defined(__loongarch__)
#  define LA
#else
#  error Unknown architecture.
#endif

typedef long ssize_t;

C void kputs(const char *s);
C void kputch(char c);

C [[noreturn]] void panic(const char *s);

#ifdef __cplusplus
C constexpr uint32_t to_big_endian(uint32_t x) {
  unsigned byte0 = x & 0xff;
  unsigned byte1 = (x >> 8) & 0xff;
  unsigned byte2 = (x >> 16) & 0xff;
  unsigned byte3 = (x >> 24) & 0xff;
  return byte3 + (byte2 << 8) + (byte1 << 16) + (byte0 << 24);
}

C constexpr uint64_t to_big_endian64(uint64_t x) {
  uint64_t byte0 = x & 0xff;
  uint64_t byte1 = (x >> 8) & 0xff;
  uint64_t byte2 = (x >> 16) & 0xff;
  uint64_t byte3 = (x >> 24) & 0xff;
  uint64_t byte4 = (x >> 32) & 0xff;
  uint64_t byte5 = (x >> 40) & 0xff;
  uint64_t byte6 = (x >> 48) & 0xff;
  uint64_t byte7 = (x >> 56) & 0xff;
  return byte7 + (byte6 << 8) + (byte5 << 16) + (byte4 << 24)
    + (byte3 << 32) + (byte2 << 40) + (byte1 << 48) + (byte0 << 56);
}
#endif

#ifdef LA
typedef enum {
  crmd   = 0x00,
  eentry = 0x0c,
  tlbidx = 0x10,
  tlbehi = 0x11,
  tlblo0 = 0x12,
  tlblo1 = 0x13,
  asid   = 0x18,
  pgdl   = 0x19,
  pgdh   = 0x1a,
  pgd    = 0x1b,
  pwcl   = 0x1c,
  pwch   = 0x1d,
  tlbrentry = 0x88,
  dmw0   = 0x180,
  dmw1   = 0x181,
  dmw2   = 0x182,
  dmw3   = 0x183,
} loongarch_csrs_t;

#define CRMD_PLV (1 << 0) /* Privilege level */
#define CRMD_IE  (1 << 2) /* Interrupt enable */
#define CRMD_DA  (1 << 3) /* Direct access */
#define CRMD_PG  (1 << 4) /* Paging */

#define TLBEHI_VPPA_SHIFT 13
/* Note this doesn't shift-out the final zeroes, unlike ptable.h. */
#define TLBEHI_VPPA(x) ((x) & (~((1ul << TLBEHI_VPPA_SHIFT) - 1)))

#define TLBLO_PPN_SHIFT 12
#define TLBLO_PPN(x) ((x) & (~((1UL << TLBLO_PPN_SHIFT) - 1)))

#define TLBLO_V      (1UL << 0)   /* Valid */ 
#define TLBLO_D      (1UL << 1)   /* Dirty (writable) */ 
#define TLBLO_PLV0   (1UL << 2)   /* OS access */ 
#define TLBLO_PLV3   (1UL << 3)   /* User access */ 
#define TLBLO_G      (1UL << 6)   /* Global */

#define TLBIDX_PS_SHIFT 24
#define TLBIDX_PS(x)  (((x) >> TLBIDX_PS_SHIFT) & 0x3ful)

#define TLB_PS_1G (0x1eul << TLBIDX_PS_SHIFT)
#define TLB_PS_2M (0x15ul << TLBIDX_PS_SHIFT)
#define TLB_PS_4K (0x0cul << TLBIDX_PS_SHIFT)
#endif

C void hexdump(const void *ptr, size_t len);

#ifdef RV
#define CSRW(reg, value) __asm__ volatile("csrw " #reg ", %0" :: "r"(value))
#define CSRR(reg, value) __asm__ volatile("csrr %0, " #reg : "=r"(value))
#define CSRS(reg, value) __asm__ volatile("csrs " #reg ", %0" :: "r"(value))
#define CSRC(reg, value) __asm__ volatile("csrc " #reg ", %0" :: "r"(value))

#define MV(reg, value) __asm__ volatile("mv " #reg ", %0" :: "r"(value))
#define RD(reg, value) __asm__ volatile("mv %0, " #reg : "=r"(value))

#define FENCE __asm__ volatile("fence" ::: "memory")
#define RFENCE __asm__ volatile("fence r, r" ::: "memory")
#define WFENCE __asm__ volatile("fence w, w" ::: "memory")
#endif

#ifdef LA
#define CSRW(reg, value) __asm__ volatile("csrwr %0, %1" :: "r"(value), "i"(loongarch_csrs_t::reg))
#define CSRR(reg, value) __asm__ volatile("csrrd %0, %1" : "=r"(value) : "i"(loongarch_csrs_t::reg))

#define MV(reg, value) __asm__ volatile ("or $" #reg ", %0, $zero" :: "r"(value))
#define RD(reg, value) __asm__ volatile ("or %0, $" #reg ", $zero" : "=r"(value))

#define FENCE __asm__ volatile("dbar 0" ::: "memory")
#define RFENCE FENCE
#define WFENCE FENCE
#endif

extern char __text_begin[], __text_end[];
extern char __rodata_begin[], __rodata_end[];
extern char __data_begin[], __data_end[];
extern char __bss_begin[], __bss_end[];
extern char __stack_top[];
extern char __kernel_begin[], __kernel_end[];

// Just all zeroes.
inline const char zeroes[4096] {};

extern reg_t rdtime();

#endif
