#include "../utils/libc.h"

void interrupt_handler(void *sp, int scause, int stval, void *sepc) {
  printf("sepc = %p, scause = %d, stval = %d\n", sepc, scause, stval);
}
