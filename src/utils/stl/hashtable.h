#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "hash.h"

extern "C" [[noreturn]] void panic(const char*);

namespace os {

// This hashmap makes the following assumption:
//   if Eq()(a, b) returns true, then Hash()(a) == Hash()(b).
//
// Moreover, if a key is present in the table, it can't be modified in some
// way that makes either Eq() or Hash() gives a different result.
//
// Finally, the hasher and equator are NOT COPIED across hash tables.
// This means they must not carry state.
template<typename K, typename V, hasher<K> Hash = detail::fnv_1a<K>, comparator<K> Eq = detail::equal<K>>
class hashmap {
public:
  enum node_state {
    Empty, Occupied, Tombstone
  };

  struct entry {
    K key;
    V value;
    node_state state = Empty;
  };
private:
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
    iterator(hashmap<K, V, Hash, Eq> *parent, size_t i): parent(parent), i(i) {
      if (i < parent->cap && parent->table[i].state != Occupied)
        advance();
    }
    iterator &operator++() { advance(); return *this; }
    iterator operator++(int) { auto it = *this; advance(); return it; }
    iterator &operator--() { retract(); return *this; }
    iterator operator--(int) { auto it = *this; retract(); return it; }
    pair<K, V&> operator*() { return { parent->table[i].key, parent->table[i].value }; }
    bool operator==(const iterator &other) const { return i == other.i; }
    bool operator!=(const iterator &other) const { return i != other.i; }
  };

  hashmap(size_t capacity = 16);
  hashmap(const hashmap &other);
  ~hashmap();
  hashmap &operator=(const hashmap &other);

  const entry *inspect_table() const { return table; }

  iterator insert(const K &key, const V &value);
  V &at(const K &key);
  iterator find(const K& key);
  bool erase(const K &key);
  void reserve(size_t len);
  bool contains(const K& key);
  int count(const K& key) { return contains(key); }
  void clear();

  V &operator[](const K &key);

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, cap); }

  size_t size() const { return sz; }
  size_t capacity() const { return cap; }
};

// We use FNV-1a. See: https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
uint64_t hashmap<K, V, Hash, Eq>::hash(const K &key) const {
  return hasher(key) % cap;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
typename hashmap<K, V, Hash, Eq>::entry* hashmap<K, V, Hash, Eq>::find_slot(const K &key) const {
  size_t start = hash(key), i = start;

  do {
    entry &current = table[i];
    if (current.state == Occupied && eq(current.key, key))
      return &current;
    if (current.state == Empty) {
      return nullptr;
    }
    i = (i + 1) % cap;
  } while (i != start);

  return nullptr;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
typename hashmap<K, V, Hash, Eq>::iterator hashmap<K, V, Hash, Eq>::insert(const K &key, const V &value) {
  if (sz >= cap / 2)
    reserve(cap * 7 / 4);

  auto start = hash(key), i = start;

  do {
    entry &cur = table[i];
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
  panic("hashtable: unreachable");
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
bool hashmap<K, V, Hash, Eq>::erase(const K& key) {
  if (entry* slot = find_slot(key)) {
    slot->state = Tombstone;
    sz--;
    return true;
  }
  return false;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
void hashmap<K, V, Hash, Eq>::reserve(size_t len) {
  if (len < cap)
    return;

  auto oldcap = cap;
  cap = len;
  auto new_table = new (safe) entry[cap];

  entry *old_table = table;
  table = new_table;
  
  sz = 0;
  for (size_t i = 0; i < oldcap; ++i) {
    const auto &[key, value, state] = old_table[i];
    if (state == Occupied)
      insert(key, value);
  }

  delete[] old_table;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::hashmap(size_t capacity) : cap(max(16ul, capacity)), sz(0) {
  table = new (safe) entry[capacity];
  for (size_t i = 0; i < capacity; i++)
    table[i].state = Empty;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::hashmap(const hashmap &other) : cap(other.cap), sz(other.sz) {
  table = new (safe) entry[cap];
  for (size_t i = 0; i < cap; i++)
    table[i] = other.table[i];
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq> &hashmap<K, V, Hash, Eq>::operator=(const hashmap &other) {
  cap = other.cap; sz = other.sz;
  table = new (safe) entry[cap];
  for (size_t i = 0; i < cap; i++)
    table[i] = other.table[i];
  return *this;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::~hashmap() {
  delete[] table;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
V &hashmap<K, V, Hash, Eq>::at(const K &key) {
  entry *slot = find_slot(key);
  assert(slot);
  return slot->value;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
bool hashmap<K, V, Hash, Eq>::contains(const K &key) {
  return (bool) find_slot(key);
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
hashmap<K, V, Hash, Eq>::iterator hashmap<K, V, Hash, Eq>::find(const K &key) {
  auto slot = find_slot(key);
  if (!slot)
    return end();
  return iterator(this, slot - table);
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
V &hashmap<K, V, Hash, Eq>::operator[](const K &key) {
  if (!contains(key))
    return (*insert(key, V())).second;

  return find_slot(key)->value;
}

template<typename K, typename V, hasher<K> Hash, comparator<K> Eq>
void hashmap<K, V, Hash, Eq>::clear() {
  delete[] table;
  table = new (safe) entry[cap = 16];
  sz = 0;
}

}

#endif
