#include "sig.h"
#include "../mem/kalloc.h"

namespace os {

void sigset::add(int signum) {
  sig |= (1 << signum);
}

void sigset::remove(int signum) {
  sig &= ~(1 << signum);
}

bool sigset::operator[](int signum) const {
  return sig & (1 << signum);
}

int sigset::next(const sigset &ignored) const {
  for (unsigned i = 1; i < sizeof(sig) * 8; i++) {
    if ((*this)[i] && !ignored[i])
      return i;
  }
  return SIGNONE;
}

void siginit() {
  auto mem = pframe();
  pmap(mem, vdso, MAP_4KB, PTE_U | PTE_G | PTE_RX, (pte_t *) as_va(__kernel_pt_root));

  // 08b00893   li a7, 139
  // 00000073   ecall
  mmwr(mem, 0x08b00893u);
  mmwr(mem + 4, 0x73u);
}

}
