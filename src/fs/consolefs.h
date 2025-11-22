#ifndef CONSOLEFS_H
#define CONSOLEFS_H

#include "vfs.h"

namespace os {

class stdin_inode : public inode {
  os::vector<char> buffer;
public:
  size_t read(size_t offset, void *buf, size_t len) override;
  size_t write(size_t, const void*, size_t) override { return 0; }
};

// Note that stdout and stderr share the same inode type.
// The buffering different is handled by libc, rather than here.
class stdout_inode : public inode {
  os::vector<char> buffer;
public:
  size_t read(size_t, void*, size_t) override { return 0; }
  size_t write(size_t offset, const void *buf, size_t len) override;
};

}

#endif
