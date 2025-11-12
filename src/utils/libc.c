#include "libc.h"
#include "helper.h"
#include <stdarg.h>

unsigned strlen(const char *s) {
  unsigned result = 0;
  while (*s++)
    result++;
  return result;
}

void *memset(void *p, int v, unsigned long size) {
  while (size--)
    *(char*)p++ = v;
  return p;
}

char *itoa(long value, char *str, int base) {
  char *p = str, *q = str;
  if (value < 0) {
    *p++ = '-'; q++;
    value = -value;
  }
  do {
    int tmp = value % base;
    *p++ = tmp < 10 ? '0' + tmp : 'a' + (tmp - 10);
  } while (value /= base);
  *p-- = '\0';

  /* Reverse the digits. */
  while (q < p) {
    char tmp = *p;
    *p-- = *q;
    *q++ = tmp;
  }
  return str;
}

int printk(const char *fmt, ...) {
  int output = 0;
  va_list args;
  va_start(args, fmt);

  char buf[32];
  for (const char *p = fmt; *p; p++) {
    if(*p != '%') {
      kputch(*p);
      output++;
      continue;
    }

    switch(*++p) {
    case 'd': {
      int val = va_arg(args, int);
      itoa(val, buf, 10);
      kputs(buf);
      output += strlen(buf);
      break;
    }
    case 'x': {
      int val = va_arg(args, int);
      itoa(val, buf, 16);
      kputs(buf);
      output += strlen(buf);
      break;
    }
    case 'p': {
      uintptr_t val = va_arg(args, uintptr_t);
      kputs("0x");
      itoa(val, buf, 16);
      kputs(buf);
      output += strlen(buf);
      break;
    }
    case 'c': {
      char val = (char)va_arg(args, int);
      kputch(val);
      output++;
      break;
    }
    case 's': {
      char *val = va_arg(args, char*);
      kputs(val);
      output += strlen(val);
      break;
    }
    case 'l':
      switch (*++p) {
      case 'd': {
        int64_t val = va_arg(args, int64_t);
        itoa(val, buf, 10);
        kputs(buf);
        output += strlen(buf);
        break;
      }
      case 'x': {
        int64_t val = va_arg(args, int64_t);
        itoa(val, buf, 16);
        kputs(buf);
        output += strlen(buf);
        break;
      }
      default:
        kputs("%l");
        break;
      }
      break;
    case '%': {
      kputch('%');
      output++;
      break;
    }
    }
  }

  va_end(args);
  return output;
}
