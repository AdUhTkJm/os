#ifndef FDT_H
#define FDT_H

#include "../utils/helper.h"

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

void read(int hart_id, header *fdt);
void *query(const char *device, const char *prop);

// Check that FDT is well-formed.
void check();

// Returns the pointer to the device tree.
header *pos();

// Read reserved memory.
vector<memrsv> reserved();

}

#endif
