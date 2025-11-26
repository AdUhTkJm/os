#ifndef ATOMIC_H
#define ATOMIC_H

#include "../helper_meta.h"
#include "../../lock/lock.h"

namespace os {

template<class T> requires is_integral_v<T>
class atomic {
  mutable spinlock lock;
  T t{};

  T load() {
    synchronized syn(lock);
    return t;
  }

  void store(T t) {
    synchronized syn(lock);
    this->t = t;
  }
public:
  /* implicit */ atomic(T t): t(t) { }
  atomic() {}

  operator T() const { return load(); }

  T fetch_add(T t) {
    synchronized syn(lock);
    auto old = t;
    this->t += t;
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
    synchronized syn(lock);
    return ++t;
  }

  T operator++(int) {
    synchronized syn(lock);
    auto before = t++;
    return before;
  }

  T operator--() {
    synchronized syn(lock);
    return --t;
  }

  T operator--(int) {
    synchronized syn(lock);
    auto before = t--;
    return before;
  }
};

};

#endif
