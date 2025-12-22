#ifndef EVENT_H
#define EVENT_H

#include "helper.h"

namespace os::event {

using on_terminate = void (*)(int pid);

extern static_storage<os::vector<on_terminate>> terminate;

void init();
void record(on_terminate f);

template<class T, class ...Args> requires requires(typename T::value_type t, Args... args) { t(args...); }
void publish(const static_storage<T> &vector, Args ...args) {
  for (auto t : *vector)
    t(args...);
}

}

#endif
