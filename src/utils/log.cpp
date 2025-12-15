#include "log.h"
#include "libc.h"

namespace {

// strcpy() that modifies dst.
void strcpy_r(char *&dst, const char *src) {
  while ((*dst++ = *src++));
}

}

// This has to be outside `os` to match declaration.
/*
C int printk(const char *fmt, ...) {
  char buf[1024];

  va_list args;
  va_start(args, fmt);

  int len = os::vsprintf(buf, fmt, args);

  va_end(args);
  sbi_console_write(len, buf);
  return len;
}
*/

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
      strcpy_r(dst, buf);
      break;
    }
    case 'u': {
      unsigned val = va_arg(args, unsigned);
      ultoa(val, buf, 10);
      strcpy_r(dst, buf);
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

      strcpy_r(dst, buf);
      break;
    }
    case 'p': {
      uintptr_t val = va_arg(args, uintptr_t);
      strcpy_r(dst, "0x");
      ultoa(val, buf, 16);
      int len = strlen(buf);

      if (zero_pad >= len) {
        for (int i = 0; i < zero_pad - len; i++)
          *dst++ = '0';
      }

      strcpy_r(dst, buf);
      break;
    }
    case 'c': {
      char val = (char)va_arg(args, int);
      *dst++ = val;
      break;
    }
    case 's': {
      char *val = va_arg(args, char*);
      strcpy_r(dst, val);
      break;
    }
    case 'l':
      switch (*++p) {
      case 'd': {
        int64_t val = va_arg(args, int64_t);
        ltoa(val, buf, 10);
        strcpy_r(dst, buf);
        break;
      }
      case 'x': {
        int64_t val = va_arg(args, int64_t);
        ltoa(val, buf, 16);
        strcpy_r(dst, buf);
        break;
      }
      default:
        strcpy_r(dst, "%l");
        break;
      }
      break;
    case '%': {
      *dst++ = '%';
      break;
    }
    }
  }
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

  log.write(tmp, len);
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

}

