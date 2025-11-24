#ifndef HASH_H
#define HASH_H

#include "../helper_meta.h"
#include "utility.h"

namespace os {

template<typename T>
concept bitwise_hashable = 
  is_scalar_v<T> || 
  (is_pod_v<T> && requires(const T& a, const T& b) {
    { a == b } -> os::same_as<bool>;
  });

template<typename T, typename K>
concept hasher = requires(const T t, K k) {
  T();
  { t(k) } -> os::same_as<uint64_t>;
};

template<typename T, typename K>
concept comparator = requires(const T t, K k) {
  T();
  { t(k, k) } -> os::same_as<bool>;
};

}

namespace os::detail {

constexpr uint64_t FNV_PRIME = 0x100000001b3ul;
constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ul;

template<typename K>
struct fnv_1a {
  uint64_t operator()(const K &key) const requires bitwise_hashable<K> {
    uint64_t hash = FNV_OFFSET_BASIS;
    unsigned char *p = (unsigned char *) &key;
    for (unsigned i = 0; i < sizeof(K); i++) {
      hash *= FNV_PRIME;
      hash ^= p[i];
    }

    return hash;
  };
};

template<>
struct fnv_1a<const char *> {
  uint64_t operator()(const char *const &key) const {
    uint64_t hash = FNV_OFFSET_BASIS;
    // Note that this iterates over the string content,
    // while the generic implementation iterates over bytes
    // of the pointer itself.
    for (unsigned char *p = (unsigned char *) key; *p; p++) {
      hash *= FNV_PRIME;
      hash ^= *p;
    }

    return hash;
  };
};

template<class T>
concept fnv_1a_hashable = requires(const T &t) { fnv_1a<T>()(t); };

template<fnv_1a_hashable T, fnv_1a_hashable U>
struct fnv_1a<pair<T, U>> {
  uint64_t operator()(const pair<T, U> &pair) const {
    const auto &[x, y] = pair;
    uint64_t hx = fnv_1a<T>()(x);
    uint64_t hy = fnv_1a<U>()(y);
    return hx * FNV_PRIME ^ hy;
  }
};

template<class K>
struct equal {
  bool operator()(const K &l, const K &r) const {
    return l == r;
  }
};

template<>
struct equal<const char*> {
  bool operator()(const char *const &l, const char * const &r) const {
    return strcmp(l, r) == 0;
  }
};

}

#endif
