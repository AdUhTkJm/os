#ifndef VECTOR_H
#define VECTOR_H

#include "utility.h"

namespace os {

template<typename V>
class vector {
  size_t cap, sz;
  V *data;
public:
  vector(): cap(0), sz(0), data(nullptr) {}
  vector(V v, size_t sz): sz(sz) {
    cap = roundup<4>(sz);
    data = (V *) vmalloc(sizeof(V) * cap);
    for (size_t i = 0; i < sz; i++)
      data[i] = v;
  }
  ~vector() { vfree(data); }
  vector(const vector &other): cap(other.cap), sz(other.sz) {
    data = (V *) vmalloc(sizeof(V) * cap);
    for (size_t i = 0; i < sz; i++)
      data[i] = other[i];
  }
  vector(vector &&other): cap(other.cap), sz(other.sz) {
    data = other.data; other.data = nullptr;
  }

  vector &operator=(const vector &other) {
    vfree(data);
    data = (V *) vmalloc(sizeof(V) * cap);
    for (size_t i = 0; i < sz; i++)
      data[i] = other[i];
  }
  vector &operator=(vector &&other) {
    cap = other.cap; sz = other.sz;
    data = other.data; other.data = nullptr;
  }

  using reference = V&;
  using const_reference = const V&;
  using iterator = V*;
  using const_iterator = const V*;

  iterator begin() { return data; }
  iterator end() { return data + sz; }

  void push_back(const V &v) {
    if (sz == cap)
      reserve(max(16ul, cap * 2));
    data[sz++] = v;
  }
  void pop_back() { sz--; }

  void reserve(size_t newcap) {
    if (newcap <= cap)
      return;

    V *newdata = (V *) vmalloc(sizeof(V) * newcap);
    for (size_t i = 0; i < sz; i++)
      newdata[i] = data[i];
    vfree(data);
    data = newdata;
    cap = newcap;
  }

  V &back() { return data[sz - 1]; }
  const V &back() const { return data[sz - 1]; }
  size_t size() const { return sz; }
  size_t capacity() const { return cap; }
  V &operator[](size_t i) { return data[i]; }
  const V &operator[](size_t i) const { return data[i]; }
  bool empty() const { return sz == 0; }
};

}

#endif
