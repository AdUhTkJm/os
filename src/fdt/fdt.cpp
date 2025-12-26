#include "fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"

namespace os::fdt {

header *pfdt;

void *query(const char *device, const char *prop) {
  char *p = (char *) pfdt + to_big_endian(pfdt->off_dt_struct);
  void *ptr = nullptr;
  auto result = walk(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    (void) len;
    if (strcmp(device, cdev) == 0 && strcmp(prop, cprop) == 0) {
      ptr = property;
      return WalkResult::Interrupt;
    }
    return WalkResult::Continue;
  });
  if (result == WalkResult::Continue && detail::read_int(p) != END)
    panic("device tree corrupted: missing end token");
  return ptr;
}

void read(int hart_id, pa_t fdt) {
  (void) hart_id;
  pfdt = (header *) as_va(fdt);
}

void check() {
  if (to_big_endian(pfdt->magic) != 0xd00dfeed)
    panic("device tree corrupted: magic number error");
  if (to_big_endian(pfdt->version) < 17)
    panic("device tree version too low");
}

header *pos() {
  return pfdt;
}

}
