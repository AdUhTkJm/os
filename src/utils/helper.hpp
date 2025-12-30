#ifndef HELPER_HPP
#define HELPER_HPP

#include "helper_meta.h"
#include "libc.h"

#include "stl/vector.h"
#include "stl/hashtable.h"
#include "stl/string.h"
#include "stl/bitmap.h"
#include "stl/list.h"
#include "stl/atomic.h"

#include "../lock/lock.h"

extern "C" [[noreturn]] void panic(const char *);

namespace os {

template<class T>
class static_storage {
private:
  alignas(T) char storage[sizeof(T)];
  bool init;
public:
  spinlock lock;

  T &operator*() {
    return *(T*) storage;
  }
  T *operator->() {
    return (T*) storage;
  }
  T *get() { return (T*) storage; }

  template<typename ...Args> requires requires(Args ...args) { T(args...); }
  void construct(Args ...args) {
    new (storage) T(args...);
    init = true;
  }

  void destroy() {
    (**this).~T();
    init = false;
  }

  bool valid() {
    return init;
  }
};

template<class T, class U> requires (is_base_of<U, T>::value)
bool isa(U *t) {
  return T::classof(t);
}

template<class T, class U> requires (is_base_of<U, T>::value)
T *cast(U *t) {
  if (!isa<T>(t))
    panic("cast: invalid cast");
  return (T*) t;
}

template<class T, class U> requires (is_base_of<U, T>::value)
T *dyn_cast(U *t) {
  if (!isa<T>(t))
    return nullptr;
  return cast<T>(t);
}

class shared {
protected:
  atomic<int> refcnt = 0;
public:
  void drop() {
#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT)
    ondrop();
#endif
    if (!--refcnt)
      delete this;
  }

  void ref() {
#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT)
    onref();
#endif
    refcnt++;
  }

#if defined(DEBUG_MEMORY) && defined(LOG_REFCNT)
  virtual void ondrop() {}
  virtual void onref() {}
  virtual ~shared() {}
#endif

#ifndef NDEBUG
  int inspect_refcnt() { return refcnt; }
#endif

  // We allow cloning, but the newly cloned element should have refcount zero.
  shared() = default;
  shared(const shared &): refcnt(0) {}
  shared &operator=(const shared &) { refcnt = 0; return *this; }
};

}

#endif
