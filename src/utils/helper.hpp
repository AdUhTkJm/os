#ifndef HELPER_HPP
#define HELPER_HPP

#include <stdint.h>

namespace os {

// Memory sizes.
// Gives the number of bytes in the given number of KB/MB/GB.
// For example, 1_mb == 1048576 (bytes).
inline uint64_t operator""_kb(unsigned long long literal) {
  return literal << 10;
}

inline uint64_t operator""_mb(unsigned long long literal) {
  return literal << 20;
}

inline uint64_t operator""_gb(unsigned long long literal) {
  return literal << 30;
}

}

#endif
