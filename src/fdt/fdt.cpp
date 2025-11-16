#include "fdt.h"
#include "../utils/libc.h"

using os::WalkResult;

namespace {

os::fdt_header_t *fdt;

const int BEGIN_NODE = 1;
const int END_NODE = 2;
const int BEGIN_PROP = 3;
const int NOP = 4;
const int END = 9;

uint32_t read_int(char *p) {
  return rev_endian(*(uint32_t *) p);
}

void skip_nop(char *&p) {
  while (read_int(p) == NOP)
    p += sizeof(uint32_t);
}

template<class T>
WalkResult parse_node(char *&p, T visitor, const char *path = "") {
  char fullpath[128];
  strcpy(fullpath, path);
  if (strcmp("/", path) != 0)
    strcat(fullpath, "/");
  skip_nop(p);

  if (read_int(p) != BEGIN_NODE)
    printk("token: %d\n", read_int(p)), panic("device tree corrupted: begin node not found");
  p += sizeof(uint32_t);

  char *device = (char *)p;
  while (*p++);
  p = os::roundup<4>(p);
  strcat(fullpath, device);

  auto stroffset = rev_endian(fdt->off_dt_strings);
  for (skip_nop(p); read_int(p) == BEGIN_PROP; skip_nop(p)) {
    p += 4;

    uint32_t len = read_int(p);
    uint32_t nameoff = read_int(p + 4);
    p += 8;

    auto result = visitor(fullpath, (char *) fdt + stroffset + nameoff, p, len);
    if (result == WalkResult::Interrupt)
      return WalkResult::Interrupt;

    p = os::roundup<4>(p + len);
  }

  for (skip_nop(p); read_int(p) != END_NODE; skip_nop(p)) {
    if (parse_node(p, visitor, fullpath) == WalkResult::Interrupt)
      return WalkResult::Interrupt;
  }
  p += 4;

  return WalkResult::Continue;
}

}

void *os::query_fdt(const char *device, const char *prop) {
  char *p = (char *) fdt + rev_endian(fdt->off_dt_struct);
  void *ptr = nullptr;
  auto result = parse_node(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    (void) len;
    if (strcmp(device, cdev) == 0 && strcmp(prop, cprop) == 0) {
      ptr = property;
      return WalkResult::Interrupt;
    }
    return WalkResult::Continue;
  });
  if (result == WalkResult::Continue && read_int(p) != END)
    panic("device tree corrupted: missing end token");
  return ptr;
}

void os::read_fdt(int hart_id, os::fdt_header_t *fdt) {
  (void) hart_id;
  if (rev_endian(fdt->magic) != 0xd00dfeed)
    panic("device tree corrupted: magic number error");
  if (rev_endian(fdt->version) < 17)
    panic("device tree version too low");
  ::fdt = fdt;
}

os::fdt_header_t *os::fdt_pos() {
  return fdt;
}
