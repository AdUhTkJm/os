#ifndef UTILITY_H
#define UTILITY_H

#include "../libc.h"
#include "../helper_meta.h"

// Placement new.
inline void *operator new(size_t, void* ptr) noexcept {
  return ptr;
}

// Normal new.
void *operator new(size_t len);
void operator delete(void *ptr, size_t);
void operator delete(void *ptr);
void *operator new[](size_t len);
void operator delete[](void *ptr);
void operator delete[](void *ptr, size_t);

namespace os {

template<class T>
constexpr T max(const T& a, const T& b) {
  return a > b ? a : b;
}

template<class T>
constexpr T min(const T& a, const T& b) {
  return a < b ? a : b;
}

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
constexpr T roundup(T x) {
  return (T) ((((uintptr_t) x) + (V - 1)) & -V);
}

template<uint64_t V, class T> requires ((V & (V - 1)) == 0)
constexpr T rounddown(T x) {
  return (T) (((uintptr_t) x) & -V);
}

template<class T, class U>
struct pair {
  T first;
  U second;

  bool operator==(const pair<T, U> &other) const {
    return first == other.first && second == other.second;
  }
};

using block = void;
using noblock = bool;

template<class T>
concept blockspec = is_same_v<T, block> || is_same_v<T, noblock>;

enum class result : bool { success, failure };

}

#endif
