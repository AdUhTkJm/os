#include "procfs.h"

namespace os {

expected<fs*> procfs_creator(const char *) {
  return new procfs;
}

}
