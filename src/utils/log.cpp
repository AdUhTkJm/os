#include "log.h"
#include "libc.h"
#include "../mem/ptable.h"

namespace {

// strcpy() that modifies dst.
void copy(char *&dst, const char *src) {
  while ((*dst++ = *src++));
  dst--; // We don't want the '\0' at the end.
}

}

// This has to be outside `os` to match declaration.

C int printk(const char *fmt, ...) {
  char buf[1024];

  va_list args;
  va_start(args, fmt);

  int len = os::vsprintf(buf, fmt, args);

  va_end(args);
  // Exclude the '\0' at the end.
  sbi_console_write(len - 1, os::to_pa(buf));
  return len;
}

namespace os {

log_buffer log;

int vsprintf(char *dst, const char *fmt, va_list args) {
  char buf[32];
  char *start = dst;
  for (const char *p = fmt; *p; p++) {
    if(*p != '%') {
      *dst++ = *p;
      continue;
    }

    /* Zero-padding. */
    int zero_pad = 0;
    if (*(p + 1) == '0')
      zero_pad = *(p += 2) - '0';

    switch(*++p) {
    case 'd': {
      int val = va_arg(args, int);
      itoa(val, buf, 10);
      copy(dst, buf);
      break;
    }
    case 'u': {
      unsigned val = va_arg(args, unsigned);
      ultoa(val, buf, 10);
      copy(dst, buf);
      break;
    }
    case 'x': {
      int val = va_arg(args, int);
      itoa(val, buf, 16);
      int len = strlen(buf);

      if (zero_pad >= len) {
        for (int i = 0; i < zero_pad - len; i++)
          *dst++ = '0';
      }

      copy(dst, buf);
      break;
    }
    case 'p': {
      uintptr_t val = va_arg(args, uintptr_t);
      copy(dst, "0x");
      ultoa(val, buf, 16);
      int len = strlen(buf);

      if (zero_pad >= len) {
        for (int i = 0; i < zero_pad - len; i++)
          *dst++ = '0';
      }

      copy(dst, buf);
      break;
    }
    case 'c': {
      char val = (char)va_arg(args, int);
      *dst++ = val;
      break;
    }
    case 's': {
      char *val = va_arg(args, char*);
      copy(dst, val);
      break;
    }
    case 'l':
      switch (*++p) {
      case 'd': {
        int64_t val = va_arg(args, int64_t);
        ltoa(val, buf, 10);
        copy(dst, buf);
        break;
      }
      case 'x': {
        int64_t val = va_arg(args, int64_t);
        ltoa(val, buf, 16);
        copy(dst, buf);
        break;
      }
      default:
        copy(dst, "%l");
        break;
      }
      break;
    case '%': {
      *dst++ = '%';
      break;
    }
    }
  }
  *dst++ = '\0';
  return dst - start;
}

int sprintf(char *dst, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int len = vsprintf(dst, fmt, args);
  va_end(args);
  return len;
}

void klog(const char *fmt, ...) {
  char tmp[256];

  va_list args;
  va_start(args, fmt);
  unsigned len = vsprintf(tmp, fmt, args);
  va_end(args);

  log.write(tmp, len - 1);
}

void log_buffer::write(const char *data, unsigned len) {
  synchronized _(lock);
  for (unsigned i = 0; i < len; i++) {
    buf[head] = data[i];
    head = (head + 1) % sizeof(buf);

    // Overwrite oldest.
    if (head == tail)
      tail = (tail + 1) % sizeof(buf);
  }
}

unsigned log_buffer::read(char *dst, unsigned len) {
  unsigned read = 0;
  synchronized _(lock);

  while (log.tail != log.head && read < len) {
    dst[read++] = log.buf[log.tail];
    log.tail = (log.tail + 1) % sizeof(log.buf);
  }
  return read;
}

unsigned log_buffer::read_all(char *dst, unsigned len) {
  unsigned read = 0;
  synchronized _(lock);

  unsigned pos = log.tail;
  unsigned available = used();

  while (available && read < len) {
    dst[read++] = log.buf[pos];
    pos = (pos + 1) % sizeof(log.buf);
    available--;
  }

  return read;
}

unsigned log_buffer::used() {
  if (log.head >= log.tail)
    return log.head - log.tail;
  return sizeof(log.buf) - (log.tail - log.head);
}

}

