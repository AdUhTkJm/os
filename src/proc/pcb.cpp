#include "pcb.h"

namespace os {

int process_file_table::allocate(file *f) {
  for (int i = 3; ; i++) {
    if (!open.count(i)) {
      open[i] = f;
      f->refcnt++;
      return i;
    }
  }
}

void process_file_table::deallocate(int fd) {
  if (!open.count(fd))
    return;
  open[fd]->refcnt--;
  open.erase(fd);
}

}