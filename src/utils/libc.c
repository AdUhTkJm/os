#include "libc.h"
#include "helper.h"
#include <stdarg.h>

unsigned strlen(const char *s) {
  unsigned result = 0;
  while (*s++)
    result++;
  return result;
}

// Must be C standard-compliant to work! g++ will assume this.
void *memset(void *p, int v, size_t size) {
  char *b = p;
  while (size--)
    *(char*) p++ = v;
  return b;
}

void *memcpy(void *dst, const void *src, size_t size) {
  char *b = dst;
  while (size--)
    *(char*) dst++ = *(char*) src++;
  return b;
}

char *itoa(int value, char *str, int base) {
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

char *ltoa(long value, char *str, int base) {
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

char *ultoa(unsigned long value, char *str, int base) {
  char *p = str, *q = str;
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

    /* Zero-padding. */
    int zero_pad = 0;
    if (*(p + 1) == '0')
      zero_pad = *(p += 2) - '0';

    switch(*++p) {
    case 'd': {
      int val = va_arg(args, int);
      itoa(val, buf, 10);
      kputs(buf);
      output += strlen(buf);
      break;
    }
    case 'u': {
      unsigned val = va_arg(args, unsigned);
      ultoa(val, buf, 10);
      kputs(buf);
      output += strlen(buf);
      break;
    }
    case 'x': {
      int val = va_arg(args, int);
      itoa(val, buf, 16);
      int len = strlen(buf);
      output += len;

      if (zero_pad >= len) {
        for (int i = 0; i < zero_pad - len; i++)
          kputch('0');
        output += zero_pad - len;
      }

      kputs(buf);
      break;
    }
    case 'p': {
      uintptr_t val = va_arg(args, uintptr_t);
      kputs("0x");
      ultoa(val, buf, 16);
      int len = strlen(buf);

      if (zero_pad >= len) {
        for (int i = 0; i < zero_pad - len; i++)
          kputch('0');
        output += zero_pad - len;
      }

      output += len;
      kputs(buf);
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
        ltoa(val, buf, 10);
        kputs(buf);
        output += strlen(buf);
        break;
      }
      case 'x': {
        int64_t val = va_arg(args, int64_t);
        ltoa(val, buf, 16);
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

int strcmp(const char *l, const char *r) {
  while (*l == *r) {
    if (*l == '\0')
      return 0;
    l++;
    r++;
  }

  return (unsigned char) *l - (unsigned char) *r;
}

void strcpy(char *dst, const char *src) {
  while ((*dst++ = *src++));
}

void strcat(char *dst, const char *src) {
  char *p = dst + strlen(dst);
  strcpy(p, src);
}

int memcmp(const void *l, const void *r, size_t n) {
  const unsigned char *a = (const unsigned char *)l;
  const unsigned char *b = (const unsigned char *)r;

  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return (int) a[i] - (int) b[i];
  }
  return 0;
}

int strncmp(const char *l, const char *r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char a = l[i];
    unsigned char b = r[i];

    if (a != b)
      return a - b;
    if (a == '\0')
      return 0;
  }
  return 0;
}

int isspace(int c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

unsigned long strtoul(const char *s, char **endptr, int base) {
  unsigned long result = 0;
  int digit;

  while (isspace(*s))
    s++;

  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
    base = 16;
  } else if (base == 0 && s[0] == '0') {
    base = 8;
  } else if (base == 0) {
    base = 10;
  }

  while (*s) {
    if (*s >= '0' && *s <= '9')
      digit = *s - '0';
    else if (*s >= 'A' && *s <= 'Z')
      digit = *s - 'A' + 10;
    else if (*s >= 'a' && *s <= 'z')
      digit = *s - 'a' + 10;
    else
      break;

    if (digit >= base)
      break;

    result = result * base + digit;
    s++;
  }

  if (endptr)
    *endptr = (char *)s;

  return result;
}

const char *strstr(const char *haystack, const char *needle) {
  if (!*needle)
    return haystack;

  for (const char *h = haystack; *h; h++) {
    if (*h == *needle) {
      const char *h_ptr = h;
      const char *n_ptr = needle;
      while (*n_ptr && *h_ptr == *n_ptr)
        h_ptr++, n_ptr++;
      if (!*n_ptr)
        return h;
    }
  }
  return NULL;
}

static size_t seed = 0x9e3779b97f4a7c15ull;
void srand(unsigned s) {
  seed = s;
}

size_t __rand64() {
  seed ^= seed >> 12;
  seed ^= seed << 25;
  seed ^= seed >> 27;
  return seed = (seed * 0x2545F4914F6CDD1Dull) >> 32;
}

int rand() {
  return __rand64();
}

int atoi(const char *str) {
  int res = 0, i = 0;
  while (str[i] && str[i] <= '9' && str[i] >= '0') {
    res = res * 10 + str[i] - '0';
    i++;
  }
  return res;
}

unsigned long atoul(const char *str) {
  unsigned long res = 0;
  int i = 0;
  while (str[i] && str[i] <= '9' && str[i] >= '0') {
    res = res * 10 + str[i] - '0';
    i++;
  }
  return res;
}

long atol(const char *str) {
  long res = 0;
  int i = 0;
  while (str[i] && str[i] <= '9' && str[i] >= '0') {
    res = res * 10 + str[i] - '0';
    i++;
  }
  return res;
}

unsigned long hextoul(const char *str) {
  unsigned long result = 0;
  for (int i = 0; str[i]; i++) {
    int digit = (
        '0' <= str[i] && str[i] <= '9' ? str[i] - 10
      : 'a' <= str[i] && str[i] <= 'f' ? str[i] - 'a' + 10
      : 'A' <= str[i] && str[i] <= 'F' ? str[i] - 'A' + 10
      : -1
    );
    if (digit == -1)
      return -1ul;
    result = result * 16 + digit;
  }
  return result;
}
