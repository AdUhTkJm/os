#ifndef LOBLOCK_H
#define LOBLOCK_H

#include "../../fs/devfs.h"

namespace os::loblock {

class lo : public block_device {
public:
  file *backup = nullptr;
  size_t offset = 0;

  bool readonly = false;
  bool autoclear = false;
  constexpr static int sectorsz = 512;
  
  int read(size_t lba, void *buf, int len) override;
  int write(size_t lba, const void *buf, int len) override;
  int sector_size() override { return sectorsz; }
};

constexpr int lomax = 3;
extern lo los[lomax];

}

#endif
