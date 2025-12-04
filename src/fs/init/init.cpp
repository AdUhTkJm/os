#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include "../../interrupt/sysret.h"
#include "../../interrupt/sysids.h"

using reg_t = long;

#ifdef __riscv
reg_t syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a4, reg_t a5, reg_t a6, reg_t a7) {
  register reg_t _a0 asm("a0") = a0;
  register reg_t _a1 asm("a1") = a1;
  register reg_t _a2 asm("a2") = a2;
  register reg_t _a3 asm("a3") = a3;
  register reg_t _a4 asm("a4") = a4;
  register reg_t _a5 asm("a5") = a5;
  register reg_t _a6 asm("a6") = a6;
  register reg_t _a7 asm("a7") = a7;

  __asm__ volatile (
    "ecall"
    : "+r"(_a0), "+r"(_a1)      // return values
    : "r"(_a2), "r"(_a3),
      "r"(_a4), "r"(_a5),
      "r"(_a6), "r"(_a7)
    : "memory"
  );
  return _a0;
}
#else
reg_t syscall(reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t, reg_t) {
  return 0; // For vscode
}
#endif

reg_t syscall(reg_t a7) {
  return syscall(0, 0, 0, 0, 0, 0, 0, a7);
}

reg_t syscall(reg_t a0, reg_t a7) {
  return syscall(a0, 0, 0, 0, 0, 0, 0, a7);
}

reg_t syscall(reg_t a0, reg_t a1, reg_t a7) {
  return syscall(a0, a1, 0, 0, 0, 0, 0, a7);
}

reg_t syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a7) {
  return syscall(a0, a1, a2, 0, 0, 0, 0, a7);
}

reg_t syscall(reg_t a0, reg_t a1, reg_t a2, reg_t a3, reg_t a7) {
  return syscall(a0, a1, a2, a3, 0, 0, 0, a7);
}

unsigned strlen(const char *s) {
  unsigned result = 0;
  while (*s++)
    result++;
  return result;
}

void kputs(const char *c) {
  syscall(/*stdout=*/ 1, (reg_t) c, strlen(c), write);
}

void kputch(char c) {
  syscall(/*stdout=*/ 1, (reg_t) &c, 1, write);
}

char kgetch() {
  char c;
  syscall(/*stdin=*/0, (reg_t) &c, 1, read);
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
  // Mount ext2.
  int ret = syscall((reg_t) "/dev/sda", (reg_t) "/mnt", (reg_t) "ext2", mount);
  (void) ret;

  int pid = syscall(clone);
  if (pid == 0) {
    // Expand heap.
    unsigned long p = syscall(0, brk);
    syscall(p + 4096, brk);

    // Move mount.
    syscall((reg_t) "/tmp", (reg_t) "/mnt/tmp", 0, 8192, mount);
    syscall((reg_t) "/dev", (reg_t) "/mnt/dev", 0, 8192, mount);

    // Switch root to ext2.
    syscall((reg_t) "/mnt", chroot);
    
    // Execute the shell.
    const char *argv[] = { "/bin/echo", "Hello World!\n", nullptr };
    syscall((reg_t) "/bin/echo", (reg_t) argv, 0, execve);

    printf("unreachable\n");
  }
  syscall(0, exit);
}
