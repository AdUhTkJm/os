#ifndef HELPER_HPP
#define HELPER_HPP

#include <stdint.h>
#include "helper_meta.hpp"

namespace os {

// Memory sizes.
// Gives the number of bytes in the given number of KB/MB/GB.
// For example, 1_mb == 1048576 (bytes).
inline constexpr uint64_t operator""_kb(unsigned long long literal) {
  return literal << 10;
}

inline constexpr uint64_t operator""_mb(unsigned long long literal) {
  return literal << 20;
}

inline constexpr uint64_t operator""_gb(unsigned long long literal) {
  return literal << 30;
}

template<uint64_t V, class T> requires ((V & (V - 1)) == 0)
T roundup(T x) {
  return (T) ((((uintptr_t) x) + (V - 1)) & -V);
}

}

#endif
