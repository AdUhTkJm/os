#include "fdt.h"
#include "../utils/libc.h"
#include "../mem/ptable.h"

using os::WalkResult;

namespace {

os::fdt::header *pfdt;

const int BEGIN_NODE = 1;
const int END_NODE = 2;
const int BEGIN_PROP = 3;
const int NOP = 4;
const int END = 9;

uint32_t read_int(char *p) {
  return to_big_endian(*(uint32_t *) p);
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

  auto stroffset = to_big_endian(pfdt->off_dt_strings);
  for (skip_nop(p); read_int(p) == BEGIN_PROP; skip_nop(p)) {
    p += 4;

    uint32_t len = read_int(p);
    uint32_t nameoff = read_int(p + 4);
    p += 8;

    auto result = visitor(fullpath, (char *) pfdt + stroffset + nameoff, p, len);
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

namespace os::fdt {

void *query(const char *device, const char *prop) {
  char *p = (char *) pfdt + to_big_endian(pfdt->off_dt_struct);
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

vector<memrsv> reserved() {
  vector<memrsv> result;
  for (auto *rsvmap = (memrsv*) ((char *) pfdt + to_big_endian(pfdt->off_mem_rsvmap));; rsvmap++) {
    if (rsvmap->address == 0 && rsvmap->size == 0)
      break;

    result.push_back(*rsvmap);
  }
  return result;
}

header *pos() {
  return pfdt;
}

}
