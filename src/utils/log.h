#ifndef LOG_H
#define LOG_H

#include "stl/ring_buffer.h"
#include <stdarg.h>

extern "C" [[gnu::no_instrument_function]] int sprintf(char *dst, const char *fmt, ...);
extern "C" [[gnu::no_instrument_function]] int vsprintf(char *dst, const char *fmt, va_list args);

namespace os {
  
void klog(const char *fmt, ...);

struct log_buffer {
  // This shouldn't be too big.
  // Kernel stackc is 8KB, and we want to place an array of the same size on stack when we are calling syslog().
  char buf[2048];
  unsigned head = 0, tail = 0;
  spinlock lock;

  void write(const char *data, unsigned len);
  // Consumes data.
  unsigned read(char *dst, unsigned len);
  // Doesn't consume data.
  unsigned read_all(char *dst, unsigned len);
  unsigned used();
} extern log;

}

#endif
