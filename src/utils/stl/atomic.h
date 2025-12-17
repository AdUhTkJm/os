#ifndef ATOMIC_H
#define ATOMIC_H

#include "../helper_meta.h"
#include "../../lock/lock.h"

namespace os {

template<class T> requires is_integral_v<T>
class atomic {
  mutable spinlock lock;
  T t{};

public:
  T load() const {
    synchronized _(lock);
    return t;
  }

  void store(T t) {
    synchronized _(lock);
    this->t = t;
  }

  /* implicit */ atomic(T t): t(t) { }
  atomic() {}
  atomic(const atomic&) = delete;
  atomic& operator=(const atomic&) = delete;
  atomic& operator=(T t) {
    store(t);
    return *this;
  }

  operator T() const { return load(); }

  T fetch_add(T v) {
    synchronized _(lock);
    auto old = t;
    t += v;
    return old;
  }

  T fetch_sub(T v) {
    synchronized _(lock);
    T old = t;
    t -= v;
    return old;
  }

  atomic<T> &operator+=(T t) {
    fetch_add(t);
    return *this;
  }

  atomic<T> &operator-=(T t) {
    fetch_add(-t);
    return *this;
  }

  T operator++() {
    synchronized _(lock);
    return ++t;
  }

  T operator++(int) {
    synchronized _(lock);
    auto before = t++;
    return before;
  }

  T operator--() {
    synchronized _(lock);
    return --t;
  }

  T operator--(int) {
    synchronized _(lock);
    auto before = t--;
    return before;
  }
};

};

#endif
