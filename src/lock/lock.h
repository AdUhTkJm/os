#ifndef LOCK_H
#define LOCK_H

#include "../utils/stl/vector.h"

namespace os {

template<class T>
concept locklike = requires(T t) {
  T();
  t.release();
  t.acquire();
};

class spinlock {
  int v = 0;
  [[gnu::no_instrument_function]] static void release_impl(int *v);
  [[gnu::no_instrument_function]] static void acquire_impl(int *v);
public:
  [[gnu::no_instrument_function]] void release() { release_impl(&v); }
  [[gnu::no_instrument_function]] void acquire() { acquire_impl(&v); }
};

template<locklike T>
class synchronized {
  T &lock;
public:
  synchronized(T &lock): lock(lock) { lock.acquire(); }
  ~synchronized() { lock.release(); }
};

}

#endif
