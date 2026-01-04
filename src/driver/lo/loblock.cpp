#include "loblock.h"

namespace os::loblock {

lo los[lomax];

int lo::read(size_t lba, void *buf, int len) {
  if (!backup)
    return -EIO;

  SeekGuard _(backup, offset + lba * sectorsz);
  auto ret = backup->read(buf, len * sectorsz);
  return ret < 0 ? ret : 0;
}

int lo::write(size_t lba, const void *buf, int len) {
  if (!backup)
    return -EIO;
  if (readonly)
    return -EACCES;
  
  SeekGuard _(backup, offset + lba * sectorsz);
  auto ret = backup->write(buf, len * sectorsz);
  return ret < 0 ? ret : 0;
}

}
