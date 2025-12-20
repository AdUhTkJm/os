#ifndef OPTIONAL_H
#define OPTIONAL_H

namespace os {

inline struct nullopt_t {} nullopt;

template<class T>
class optional {
  bool has_value;
  alignas(T) char data[sizeof(T)];

  T* ptr() { return reinterpret_cast<T*>(data); }
  const T* ptr() const { return reinterpret_cast<const T*>(data); }
public:
  optional(const optional&) = delete;
  optional& operator=(const optional&) = delete;

  /* implicit */ optional(const T &t): has_value(true) {
    new (data) T(t);
  }
  optional(T &&t): has_value(true) {
    new (data) T((T &&) t);
  }

  template<class ...Args> requires requires(Args ...args) { T(args...); }
  void emplace(Args ...args) {
    new (data) T(args...);
  }

  optional(nullopt_t): has_value(false) {}
  optional(): has_value(false) {}
  ~optional() {
    if (has_value)
      ptr()->~T();
  }

  optional(optional &&other): has_value(other.has_value) {
    if (other.has_value) {
      new (data) T((T &&) *other.ptr());
      other.ptr()->~T();
      other.has_value = false;
    }
  }

  // Move assignment
  optional& operator=(optional &&other) {
    if (this == &other)
      return *this;
    if (has_value)
      ptr()->~T();
    
    has_value = other.has_value;
    if (other.has_value) {
      new (data) T((T &&) *other.ptr());
      other.ptr()->~T();
      other.has_value = false;
    }
    return *this;
  }

  T &operator*() { return *(T*) data; }
  T *operator->() { return (T*) data; }
  const T &operator*() const { return *(T*) data; }
  const T *operator->() const { return (T*) data; }
  bool valid() const { return has_value; }
  bool operator!() const { return !has_value; }
  explicit operator bool() const { return has_value; }
};

template<class T, class E = int>
class expected {
  E errcode;
  bool has_value;
  alignas(T) char data[sizeof(T)];

  T* ptr() { return reinterpret_cast<T*>(data); }
  const T* ptr() const { return reinterpret_cast<const T*>(data); }
public:
  expected(const expected&) = delete;
  expected& operator=(const expected&) = delete;

  /* implicit */ expected(const T &t): errcode(), has_value(true) {
    new (data) T(t);
  }
  expected(T &&t): errcode(), has_value(true) {
    new (data) T((T &&) t);
  }

  template<class ...Args> requires requires(Args ...args) { T(args...); }
  void emplace(Args ...args) {
    errcode = E();
    new (data) T(args...);
  }

  expected(E c): errcode(c), has_value(false) {}
  expected(E c, nullopt_t): errcode(c), has_value(false) {}
  expected(): errcode(), has_value(false) {}
  ~expected() {
    if (has_value)
      ptr()->~T();
  }

  expected(expected &&other): errcode(other.errcode), has_value(other.has_value) {
    if (other.has_value) {
      new (data) T((T &&) *other.ptr());
      other.ptr()->~T();
      other.has_value = false;
    }
  }

  // Move assignment
  expected &operator=(expected &&other) {
    if (this == &other)
      return *this;
    if (has_value)
      ptr()->~T();
    
    has_value = other.has_value;
    errcode = other.errcode;
    if (other.has_value) {
      new (data) T((T &&) *other.ptr());
      other.ptr()->~T();
      other.has_value = false;
    }
    return *this;
  }

  T &operator*() { return *(T*) data; }
  T *operator->() { return (T*) data; }
  T *operator&() { return (T*) data; }
  const T &operator*() const { return *(T*) data; }
  const T *operator->() const { return (T*) data; }
  const T *operator&() const { return (T*) data; }
  bool valid() const { return has_value; }
  bool operator!() const { return !has_value; }
  operator E() const { return errcode; }
  E error() const { return errcode; }
  explicit operator bool() const { return has_value; }
};

}

#endif
