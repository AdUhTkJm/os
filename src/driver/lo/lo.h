#ifndef LO_H
#define LO_H

#include "../../fs/net.h"

namespace os::lo {

class lo : public net_device {
public:
  lo() = default;
  lo(const lo &) = delete;
  net_device &operator=(const lo &) = delete;

  int write(const void *buf, int len, bool block) override;
  bool write_full() override { return false; }
  void wake_write() override {}
} extern localhost;

}

#endif
