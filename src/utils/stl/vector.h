#ifndef VECTOR_H
#define VECTOR_H

#include "utility.h"

namespace os {

template<typename V>
class vector {
  size_t cap, sz;
  V *dat;
public:
  vector(): cap(0), sz(0), dat(nullptr) {}
  vector(V v, size_t sz): sz(sz) {
    cap = roundup<4>(sz);
    dat = new (safe) V[cap];
    for (size_t i = 0; i < sz; i++)
      dat[i] = v;
  }
  ~vector() { delete[] dat; }
  vector(const vector &other): cap(other.cap), sz(other.sz) {
    dat = new (safe) V[cap];
    for (size_t i = 0; i < sz; i++)
      dat[i] = other[i];
  }
  vector(vector &&other): cap(other.cap), sz(other.sz) {
    dat = other.dat; other.dat = nullptr;
  }

  vector &operator=(const vector &other) {
    delete[] dat;
    dat = new V[cap];
    sz = other.sz;
    cap = other.cap;
    for (size_t i = 0; i < sz; i++)
      dat[i] = other[i];
    return *this;
  }
  vector &operator=(vector &&other) {
    cap = other.cap; sz = other.sz;
    dat = other.dat; other.dat = nullptr;
    return *this;
  }

  using reference = V&;
  using const_reference = const V&;
  using iterator = V*;
  using const_iterator = const V*;

  iterator begin() { return dat; }
  iterator end() { return dat + sz; }

  const_iterator begin() const { return dat; }
  const_iterator end() const { return dat + sz; }

  void push_back(const V &v) {
    if (sz == cap)
      reserve(max(16ul, cap * 2));
    dat[sz++] = v;
  }
  void pop_back() { sz--; }

  void reserve(size_t newcap) {
    if (newcap <= cap)
      return;

    V *newdata = new (safe) V[newcap];
    for (size_t i = 0; i < sz; i++)
      newdata[i] = dat[i];
    delete[] dat;
    dat = newdata;
    cap = newcap;
  }

  void resize(size_t newsz) {
    if (sz > newsz) {
      sz = newsz;
      return;
    }
    reserve(newsz);
    sz = newsz;
  }

  void insert(V *place, V element) {
    long d = place - dat;
    if (sz == cap)
      reserve(max(16ul, cap * 2));
    // It is possible that d is 0, so we go signed here.
    for (long i = sz; i > d; i--)
      dat[i] = dat[i - 1];
    dat[d] = element;
    sz++;
  }

  void erase(V *place) {
    auto d = place - dat;
    for (long i = d; i < long(sz); i++)
      dat[i] = dat[i + 1];
    sz--;
  }

  void clear() {
    sz = 0;
  }

  V *data() { return dat; }
  const V* data() const { return dat; }

  V &back() { return dat[sz - 1]; }
  const V &back() const { return dat[sz - 1]; }

  size_t size() const { return sz; }
  size_t capacity() const { return cap; }

  V &operator[](size_t i) { return dat[i]; }
  const V &operator[](size_t i) const { return dat[i]; }

  bool empty() const { return sz == 0; }
};

}

#endif
