#ifndef LOG_H
#define LOG_H

#include "stl/ring_buffer.h"
#include <stdarg.h>

namespace os {

int sprintf(char *dst, const char *fmt, ...);
int vsprintf(char *dst, const char *fmt, va_list args);
void klog(const char *fmt, ...);

struct log_buffer {
  char buf[8192];
  unsigned head = 0, tail = 0;
  spinlock lock;

  void write(const char *data, unsigned len);
} extern log;

}

#endif
