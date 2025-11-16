#ifndef HELPER_HPP
#define HELPER_HPP

#include "helper_meta.h"
#include "libc.h"

#include "stl/vector.h"
#include "stl/hashtable.h"
#include "stl/string.h"
#include "stl/bitmap.h"

namespace os {


template<class T>
class static_storage {
private:
  alignas(T) char storage[sizeof(T)];
  bool init = false;
public:
  T &operator*() {
    return *(T*) storage;
  }

  template<typename ...Args>
  void construct(Args ...args) {
    new (storage) T(args...);
    init = true;
  }

  void destroy() {
    (**this).~T();
    init = false;
  }
};

}

#endif
