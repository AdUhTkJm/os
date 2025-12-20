#include "futex.h"
#include "../proc/schedule.h"

namespace os {

static_storage<os::hashmap<futex_key, futex_queue*>> futexes;

bool futex_key::operator==(const futex_key &other) const {
  if (type != other.type)
    return false;
  if (type == PRIVATE)
    return priv.addr == other.priv.addr && priv.mm == other.priv.mm;
  return shared.offset == other.shared.offset && shared.node == other.shared.node;
}

futex_key::futex_key(va_t addr) {
  auto tcb = active();
  auto pcb = tcb->pcb;

  auto i = pcb->vma.find(addr);
  if (i == pcb->vma.size()) {
    type = BAD;
    return;
  }

  const auto &vma = pcb->vma[i];
  if (vma.flags & MAP_SHARED) {
    type = SHARED;
    shared.node = vma.backup->node();
    shared.offset = vma.offset;
  } else {
    type = PRIVATE;
    priv.addr = addr;
    priv.mm = &pcb->vma;
  }
}

}
