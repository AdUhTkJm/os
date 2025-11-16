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

template<typename K>
struct fnv_1a {
  uint64_t operator()(const K &key) const requires bitwise_hashable<K> {
    const uint64_t FNV_PRIME = 0x100000001b3ul;
    const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ul;

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
    const uint64_t FNV_PRIME = 0x100000001b3ul;
    const uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ul;

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
