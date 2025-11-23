#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"
#include "../proc/pcb.h"
#include "../utils/stl/ring_buffer.h"

namespace os {

class console_inode : public inode {
  os::list<pcb_t *> wait;
  spinlock lock;
public:
  size_t read(size_t offset, void *buf, size_t len) override;
  size_t write(size_t, const void*, size_t) override;

  void wake();
};

extern static_storage<console_inode> tty0;

}

#endif
