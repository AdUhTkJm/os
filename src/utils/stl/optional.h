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
};

}

#endif
