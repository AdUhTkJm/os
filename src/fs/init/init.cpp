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

constexpr unsigned short htons(unsigned short x) {
  unsigned byte0 = x & 0xff;
  unsigned byte1 = (x >> 8) & 0xff;
  return byte1 + (byte0 << 8);
}

extern "C" void _start() {
  constexpr int stdin = 0, stdout = 1, stderr = 2;

  // Mount ext2.
  syscall((reg_t) "/dev/vdb", (reg_t) "/mnt", (reg_t) "ext2", mount);
  // Move mount.
  syscall((reg_t) "/tmp", (reg_t) "/mnt/tmp", 0, /*MS_MOVE=*/8192, mount);
  syscall((reg_t) "/dev", (reg_t) "/mnt/dev", 0, /*MS_MOVE=*/8192, mount);

  // Switch root to ext2.
  syscall((reg_t) "/mnt", chroot);

  // Change current directory.
  syscall((reg_t) "/", chdir);

  // Mount proc, and the ext4 for testing (which is also ext2).
  syscall((reg_t) "", (reg_t) "/proc", (reg_t) "procfs", mount);
  syscall((reg_t) "/dev/vda", (reg_t) "/mnt", (reg_t) "ext2", mount);

  int pid = syscall(0x11, 0, clone);
  if (pid == 0) {
    // Redirect stdin/stdout/stderr to tty.
    int in = syscall(-1, (reg_t) "/dev/tty", /*O_RDONLY=*/0, 0, openat);
    int out = syscall(-1, (reg_t) "/dev/tty", /*O_WRONLY=*/1, 0, openat);
    syscall(in, stdin, 0, dup3);
    syscall(out, stdout, 0, dup3);
    syscall(out, stderr, 0, dup3);

    // This process should be in its own group.
    syscall(0, 0, setpgid);

    // Set tty leader to this process.
    int pgid = syscall(0, getpgid);
    syscall(stdin, /*TIOCSPGRP=*/ 0x5410, (reg_t) &pgid, ioctl);
    
    // Execute the shell.
#if defined(TEST)
#define LIBC "glibc"
#define CD "cd /mnt/" LIBC
    [[gnu::unused]] const char *ltp = R"( 
cd /mnt/glibc/ltp/testcases/bin
echo "#### OS COMP TEST GROUP START ltp-$1 ####"
for f in *; do
  [ -f "$f" ] || continue
  case "$f" in
      *.sh) continue ;;
  esac

  "./$f"
done
echo "#### OS COMP TEST GROUP END ltp-$1 ####"
)";
    const char *test = 
      // CD "/basic && sh ./run-all.sh";
      // CD " && sh ./busybox_testcode.sh";
      // CD " && sh ./libctest_testcode.sh";
      // CD " && sh ./libcbench_testcode.sh";
      // CD " && sh ./unixbench_testcode.sh";
      // CD " && sh ./lmbench_testcode.sh";
      // CD " && sh ./iozone_testcode.sh";
      // CD " && sh ./cyclictest_testcode.sh";
      // CD " && sh ./iperf_testcode.sh";
      // ltp;
      CD "/ltp/testcases/bin && ./access04; echo 'done'";
    const char *argv[] = { "/bin/sh", "-c", test, nullptr };
#elif defined(REMOTE_TEST)
    const char *test = R"(
single() {
  chmod +x basic/run-all.sh
  sh ./basic_testcode.sh
  sh ./busybox_testcode.sh
  sh ./libcbench_testcode.sh

  # sh ./lmbench_testcode.sh
  cd ltp/testcases/bin
  echo "#### OS COMP TEST GROUP START ltp-$1 ####"
  for f in *; do
    [ -f "$f" ] || continue
    case "$f" in
        *.sh) continue ;;
    esac

    "./$f"
  done
  echo "#### OS COMP TEST GROUP END ltp-$1 ####"
}

cd /mnt/glibc
single glibc
# cd /mnt/musl
# single musl
halt -f
)";
    const char *argv[] = { "/bin/sh", "-c", test, nullptr };
#else
    const char *argv[] = { "/bin/sh", nullptr };
#endif
    const char *envp[] = { "PATH=/bin:/usr/bin:/sbin", "HOME=/root", "LC_ALL=C", "LANG=C", nullptr }; 
    syscall((reg_t) "/bin/sh", (reg_t) argv, (reg_t) envp, execve);

    __builtin_unreachable();
  }

  // Keep waiting for zombie processes.
  for (;;) {
    // Don't keep waiting if there's no other processes (-ECHILD).
    if (syscall(-1, 0, 0, 0, wait4) == -10) {
      timespec rqtp { .tv_sec = 10, .tv_nsec = 0 };
      syscall((reg_t) &rqtp, 0, nanosleep);
    }
  }
}
