#ifndef LOCK_H
#define LOCK_H

#include "../utils/stl/list.h"

#ifdef DEADLOCK
extern "C" [[noreturn]] void panic(const char *);
extern "C" int printk(const char *, ...);
#include "../instr/stack.h"
#endif

namespace os {

namespace detail {

inline int nested_irq = 0;

}

// Note we can't write to sstatus.SIE bit.
#if defined(__riscv) || IN_VSCODE
inline void disable_preempt() {
  if (detail::nested_irq++ == 0) {
    __asm__ volatile(
      "csrc sie, 2\n"   // software interrupts
      "csrc sie, %0\n"  // timer interrupts
      "csrc sie, %1\n"  // external interrupts 
    :: "r"(32), "r"(512));
  }
}

inline void enable_preempt() {
  if (--detail::nested_irq == 0) {
    __asm__ volatile(
      "csrs sie, 2\n"
      "csrs sie, %0\n"
      "csrs sie, %1\n"
    :: "r"(32), "r"(512));
  }
}
#endif
#if defined(__loongarch__)
inline void disable_preempt() {
  unsigned crmd;
  CSRR(crmd, crmd);
  crmd &= ~CRMD_IE;
  CSRW(crmd, crmd);
}

inline void enable_preempt() {
  unsigned crmd;
  CSRR(crmd, crmd);
  crmd |= CRMD_IE;
  CSRW(crmd, crmd);
}

#endif

#ifndef UNIPROCESSOR
struct nopreempt {
  nopreempt() { disable_preempt(); }
  ~nopreempt() { enable_preempt(); }
};
#else
// Does entirely nothing.
// sstatus.SIE is automatically disabled on interrupt handler,
// so no other interrupts can ever fire.
struct nopreempt {};
#endif

template<class T>
concept locklike = requires(T t) {
  T();
  t.release();
  t.acquire();
};

#ifdef DEADLOCK
struct tcb_t;
tcb_t *active();
#endif

// Similarly, a spinlock just does nothing in uniprocessor kernel.
class spinlock {
  int v = 0;
  [[gnu::no_instrument_function]] static void release_impl(int *v);
  [[gnu::no_instrument_function]] static void acquire_impl(int *v);

#ifdef DEADLOCK
  tcb_t *owner = nullptr;
#  ifdef FUNC_INSTRUMENT
  stack::shadow_stack stack;
#  endif
#endif
public:
  spinlock() = default;
  spinlock(const spinlock &other) = delete;
  spinlock &operator=(const spinlock &other) = delete;

#ifdef DEADLOCK
  [[gnu::noinline]] 
#endif
  [[gnu::no_instrument_function]] void release() {
#ifdef DEADLOCK
    if (!owner) {
#  ifdef FUNC_INSTRUMENT
      printk("lock: last released:");
      stack::dump(stack);
      printk("lock: now:");
      stack::dump();
#  endif
      panic("lock: release: double release");
    }
    // We shouldn't check whether owner == active().
    // The lock in scheduler will change active process.
    owner = nullptr;
#  ifdef FUNC_INSTRUMENT
    stack::copy(&stack);
#  endif
#endif
#ifndef UNIPROCESSOR
    release_impl(&v);
#endif
    enable_preempt();
  }

#ifdef DEADLOCK
  [[gnu::noinline]] 
#endif
  [[gnu::no_instrument_function]] void acquire() {
#ifdef DEADLOCK
    if (owner == active()) {
#  ifdef FUNC_INSTRUMENT
      printk("lock: last acquired:\n");
      stack::dump(stack);
      printk("lock: now:\n");
      stack::dump();
#  endif
      panic("lock: acquire: already owned");
    }
    owner = active();
#  ifdef FUNC_INSTRUMENT
    stack::copy(&stack);
#  endif
#endif
    disable_preempt();
#ifndef UNIPROCESSOR
    acquire_impl(&v);
#endif
  }
};

template<locklike T>
class synchronized {
  T &lock;
public:
  synchronized(T &lock): lock(lock) { lock.acquire(); }
  ~synchronized() { lock.release(); }
};

struct tcb_t;
struct wait_entry : intrusive_list_node<wait_entry> {
  tcb_t *tcb;
  bool queued = false;
};

struct wait_queue {
  spinlock lock;
  os::intrusive_list<wait_entry> q;

  void prepare(wait_entry &entry);
  void finish(wait_entry &entry);
  int wake_all();
  int wake(int n = 1);
};

}

#endif
