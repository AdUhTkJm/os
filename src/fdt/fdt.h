#ifndef FDT_H
#define FDT_H

#include "../utils/helper.h"
#include "../mem/ptable.h"

namespace os {

enum class WalkResult {
  Interrupt, Continue
};

}

namespace os::fdt {

/*
For specification, see https://devicetree-specification.readthedocs.io/en/latest.

Note that everything in the header is stored in big-endian format.
*/
typedef struct {
  uint32_t magic;
  uint32_t totalsize;
  uint32_t off_dt_struct;
  uint32_t off_dt_strings;
  uint32_t off_mem_rsvmap;
  uint32_t version;
  uint32_t last_comp_version;
  uint32_t boot_cpuid_phys;
  uint32_t size_dt_strings;
  uint32_t size_dt_struct;
} header;

typedef struct {
  uint64_t address;
  uint64_t size;
} memrsv;

void read(int hart_id, pa_t fdt);
void *query(const char *device, const char *prop);

// Check that FDT is well-formed.
void check();

// The pointer to the device tree root.
extern os::fdt::header *pfdt;

const int BEGIN_NODE = 1;
const int END_NODE = 2;
const int BEGIN_PROP = 3;
const int NOP = 4;
const int END = 9;

namespace detail {

inline uint32_t read_int(void *p) {
  return to_big_endian(*(uint32_t *) p);
}

inline void skip_nop(char *&p) {
  while (read_int(p) == NOP)
    p += sizeof(uint32_t);
}

}

template<class T>
concept fdt_walker = requires (T t, const char *cdev, const char *cprop, void *property, int len) {
  { t(cdev, cprop, property, len) } -> same_as<WalkResult>;
};

template<fdt_walker T> 
WalkResult walk(char *&p, T visitor, const char *path = "") {
  using namespace detail;

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
    if (walk(p, visitor, fullpath) == WalkResult::Interrupt)
      return WalkResult::Interrupt;
  }
  p += 4;
  return WalkResult::Continue;
}

}

#endif
