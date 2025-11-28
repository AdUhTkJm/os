#ifndef OPTIONAL_H
#define OPTIONAL_H

namespace os {

inline struct nullopt_t {} nullopt;

template<class T>
class optional {
  bool present;
  alignas(T) char data[sizeof(T)];
public:
  /* implicit */ optional(const T &t): present(true) {
    new (data) T(t);
  }
  optional(): present(false) {}
  optional(nullopt_t): present(false) {}

  T &operator*() { return *(T*) data; }
  T *operator->() { return (T*) data; }
  T *operator&() { return (T*) data; }
  bool valid() const { return present; }
  bool operator!() const { return !present; }
  operator bool() const { return present; }
};

template<class T>
class errable {
  int errcode;
  alignas(T) char data[sizeof(T)];
public:
  /* implicit */ errable(const T &t): errcode(0) {
    new (data) T(t);
  }
  errable(int c): errcode(c) {}

  T &operator*() { return *(T*) data; }
  T *operator->() { return (T*) data; }
  T *operator&() { return (T*) data; }
  bool valid() const { return errcode == 0; }
  bool operator!() const { return errcode != 0; }
  operator int() const { return errcode; }
};

}

#endif
