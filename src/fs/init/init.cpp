#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

using reg_t = long;

[[gnu::naked]]
reg_t syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a4, reg_t a5, reg_t a6, reg_t a7) {
  __asm__ volatile(
    "ecall\n"
    "ret\n"
  ::: "memory");
}

[[gnu::naked]]
reg_t syscall(reg_t a7) {
  __asm__ volatile(
    "mv a7, a0\n"
    "ecall\n"
    "ret\n"
  ::: "memory");
}

[[gnu::naked]]
reg_t syscall(reg_t a0, reg_t a7) {
  __asm__ volatile(
    "mv a7, a1\n"
    "ecall\n"
    "ret\n"
  ::: "memory");
}

[[gnu::naked]]
reg_t syscall(reg_t a0, reg_t a1, reg_t a7) {
  __asm__ volatile(
    "mv a7, a2\n"
    "ecall\n"
    "ret\n"
  ::: "memory");
}

[[gnu::naked]]
reg_t syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a7) {
  __asm__ volatile(
    "mv a7, a3\n"
    "ecall\n"
    "ret\n"
  ::: "memory");
}

unsigned strlen(const char *s) {
  unsigned result = 0;
  while (*s++)
    result++;
  return result;
}

void kputs(const char *c) {
  syscall(/*stdout=*/ 1, (reg_t) c, strlen(c), /*write*/ 1);
}

void kputch(char c) {
  syscall(/*stdout=*/ 1, (reg_t) &c, 1, /*write*/ 1);
}

char kgetch() {
  char c;
  syscall(/*stdin=*/0, (reg_t) &c, 1, /*read*/0);
  return c;
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

char *itoa_u(unsigned long value, char *str, int base) {
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

int printf(const char *fmt, ...) {
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
    case 'u': {
      unsigned val = va_arg(args, unsigned);
      itoa_u(val, buf, 10);
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
      itoa_u(val, buf, 16);
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

extern "C" void _start() {
  int pid = syscall(57);
  printf("forked, pid = %d\n", pid);
  if (pid == 0) {
    char c = kgetch();
    printf("input = %c\n", c);
    printf("about to exit\n");
    syscall(0, 60);
  }
  syscall(0, 60);
}
