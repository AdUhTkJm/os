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
  static void release_impl(int *v);
  static void acquire_impl(int *v);
public:
  void release() { release_impl(&v); }
  void acquire() { acquire_impl(&v); }
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
