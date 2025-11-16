#ifndef FDT_H
#define FDT_H

#include "../utils/helper.h"

namespace os {

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
} fdt_header_t;

typedef struct {
  uint64_t address;
  uint64_t size;
} fdt_memrsv_t;

void read_fdt(int hart_id, fdt_header_t *fdt);
void *query_fdt(const char *device, const char *prop);
fdt_header_t *fdt_pos();

enum class WalkResult {
  Interrupt, Continue
};

template<class T> requires requires(char *device, char *propname, void *property, int len) {
  { os::declval<T>()(device, propname, property, len) } -> os::same_as<WalkResult>;
}
void query_fdt(T visitor);

}

#endif
