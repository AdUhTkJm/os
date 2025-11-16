#ifndef HELPER_HPP
#define HELPER_HPP

#include <stdint.h>
#include <stddef.h>
#include "helper_meta.hpp"
#include "libc.h"

// Placement new.
inline void* operator new(size_t, void* ptr) noexcept {
  return ptr;
}

namespace os {

void *vmalloc(size_t len);
void vfree(void *p);

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
};

template<size_t Size>
class bitmap {
public:
  using unit = unsigned;
  constexpr static size_t unit_bits = sizeof(unit) * 8;
  constexpr static size_t size = Size;

  class reference {
  private:
    // The element in the map.
    unit &elem;
    const size_t bitpos;
  public:
    reference(unit &u, size_t index): elem(u), bitpos(index) {}
    reference &operator=(bool value) {
      if (value)
        elem |= (1u << bitpos);
      else
        elem &= ~(1u << bitpos);
      return *this;
    }
    operator bool() const {
      return (elem & (1U << bitpos)) != 0;
    }
    bool operator!() const {
      return !(bool) *this;
    }
  };

  class const_reference {
  private:
    // The element in the map.
    unit elem;
    const size_t bitpos;
  public:
    const_reference(unit u, size_t index): elem(u), bitpos(index) {}
    operator bool() const {
      return (elem & (1U << bitpos)) != 0;
    }
    bool operator!() const {
      return !(bool) *this;
    }
  };

  reference operator[](size_t i) {
    return reference(map[i / unit_bits], i % unit_bits);
  }

  const_reference operator[](size_t i) const {
    return const_reference(map[i / unit_bits], i % unit_bits);
  }

  void zero() {
    memset(map, 0, sizeof(map));
  }
private:
  unit map[os::roundup<unit_bits>(Size)];
};

template<class T>
class static_storage {
private:
  alignas(T) char storage[sizeof(T)];
  bool init = false;
public:
  T &operator*() {
    return *(T*) storage;
  }

  template<typename ...Args>
  void construct(Args ...args) {
    new (storage) T(args...);
    init = true;
  }

  void destroy() {
    (**this).~T();
    init = false;
  }
};


template<typename T>
concept hashmap_valid_key = 
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

namespace detail {

template<class K>
struct fnv_1a {
  uint64_t operator()(const K &key) const {
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

// This hashmap makes the following assumption:
//   if Eq()(a, b) returns true, then Hash()(a) == Hash()(b).
//
// Moreover, if a key is present in the table, it can't be modified in some
// way that makes either Eq() or Hash() gives a different result.
template<hashmap_valid_key K, typename V, hasher<K> Hash = detail::fnv_1a<K>, comparator<K> Eq = detail::equal<K>>
class hashmap {
  enum node_state {
    Empty, Occupied, Tombstone
  };

  struct entry {
    K key;
    V value;
    node_state state;
  };

  // We're using a hash table with linear probing.
  entry* table = nullptr;
  size_t cap = 0;
  size_t sz = 0;
  Hash hasher;
  Eq eq;

  uint64_t hash(const K& key) const;
  entry* find_slot(const K& key) const;
public:
  using key_type = K;
  using value_type = pair<K, V>;

  class iterator {
    hashmap<K, V, Hash, Eq> *parent = nullptr;
    size_t i = 0;

    void advance() {
      i++;
      while (i < parent->cap) {
        if (parent->table[i].state == Occupied)
          break;
        i++;
      }
    }
    
    void retract() {
      while (i != -1ul) {
        if (parent->table[i].state == Occupied)
          break;
        i--;
      }
    }
  public:
    iterator(hashmap<K, V> *parent, size_t i): parent(parent), i(i) {
      if (i < parent->cap && parent->table[i].state != Occupied)
        advance();
    }
    iterator &operator++() { advance(); return *this; }
    iterator operator++(int) { auto it = *this; advance(); return it; }
    iterator &operator--() { retract(); return *this; }
    iterator operator--(int) { auto it = *this; retract(); return it; }
    pair<K, V&> operator*() { return { parent->table[i].key, parent->table[i].value }; }
    pair<K, V&> operator->() { return {}; }
    bool operator==(const iterator &other) const { return i == other.i; }
    bool operator!=(const iterator &other) const { return i != other.i; }
  };

  hashmap(size_t capacity = 16);
  ~hashmap();

  iterator insert(const K &key, const V &value);
  V &at(const K &key);
  bool erase(const K &key);
  void reserve(size_t len);
  bool contains(const K& key);
  int count(const K& key) { return contains(key); }

  V &operator[](const K &key);

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, cap); }

  size_t size() const { return sz; }
  size_t capacity() const { return cap; }
};

// We use FNV-1a. See: https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
uint64_t hashmap<K, V, Hash, Eq>::hash(const K &key) const {
  return hasher(key) % cap;
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
typename hashmap<K, V, Hash, Eq>::entry* hashmap<K, V, Hash, Eq>::find_slot(const K &key) const {
  size_t start = hash(key), i = start;

  do {
    entry& current = table[i];
    if (current.state == Occupied && eq(current.key, key))
      return &current;
    if (current.state == Empty) {
      return nullptr;
    }
    i = (i + 1) % cap;
  } while (i != start);

  return nullptr;
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
typename hashmap<K, V, Hash, Eq>::iterator hashmap<K, V, Hash, Eq>::insert(const K &key, const V &value) {
  if (sz >= cap * 3 / 4)
    reserve(cap * 2);

  auto start = hash(key), i = start;

  do {
    entry& cur = table[i];
    if (cur.state == Occupied && eq(cur.key, key)) {
      cur.value = value;
      return iterator(this, i);
    }  
    if (cur.state == Tombstone || cur.state == Empty) {
      table[i] = entry { key, value, Occupied };
      sz++;
      return iterator(this, i);
    }

    i = (i + 1) % cap;
  } while (i != start);
  // unreachable.
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
bool hashmap<K, V, Hash, Eq>::erase(const K& key) {
  if (entry* slot = find_slot(key)) {
    slot->state = Tombstone;
    sz--;
    return true;
  }
  return false;
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
void hashmap<K, V, Hash, Eq>::reserve(size_t len) {
  if (len < cap)
    return;

  auto oldcap = cap;
  cap = len;
  auto new_table = (entry*) vmalloc(cap * sizeof(entry));
  for (size_t i = 0; i < cap; ++i)
    new_table[i].state = Empty;

  entry* old_table = table;
  table = new_table;
  
  for (size_t i = 0; i < oldcap; ++i) {
    const auto& [key, value, state] = old_table[i];
    if (state == Occupied)
      insert(key, value);
  }

  vfree(old_table);
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::hashmap(size_t capacity) : cap(max(16ul, capacity)), sz(0) {
  table = (entry*) vmalloc(capacity * sizeof(entry));
  for (size_t i = 0; i < capacity; ++i)
    table[i].state = Empty;
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::~hashmap() {
  vfree(table);
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
V &hashmap<K, V, Hash, Eq>::at(const K &key) {
  return find_slot(key)->value;
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
bool hashmap<K, V, Hash, Eq>::contains(const K &key) {
  return (bool) find_slot(key);
}

template<hashmap_valid_key K, typename V, hasher<K> Hash, comparator<K> Eq>
V &hashmap<K, V, Hash, Eq>::operator[](const K &key) {
  if (!contains(key))
    return (*insert(key, V())).second;

  return find_slot(key)->value;
}

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

  using reference = V&;
  using const_reference = const V&;
  using iterator = V*;
  using const_iterator = const V*;

  iterator begin() { return data; }
  iterator end() { return data + sz; }

  void push_back(const V &v) {
    if (sz == cap)
      reserve(cap < 16 ? 16 : cap * 2);
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
};

}

#endif
