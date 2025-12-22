#ifndef STRING_H
#define STRING_H

#include "utility.h"
#include "hash.h"
#include "vector.h"

namespace os {

class string {
  static constexpr size_t SSO_CAP = 24;
  union {
    char sso[SSO_CAP];
    char *heap;
  } u;
  size_t len;

  bool small() const { return len < SSO_CAP; }
  bool large() const { return len >= SSO_CAP; }
public:
  constexpr static size_t npos = -1ul;

  string(): u({ .heap = nullptr }), len(0) {}

  /* implicit */ string(const char *q): len(strlen(q)) {
    if (small()) {
      memcpy(u.sso, q, len + 1);
    } else {
      u.heap = new char[len + 1];
      memcpy(u.heap, q, len + 1);
    }
  }

  string(const char *q, size_t sz): len(sz) {
    if (small()) {
      memcpy(u.sso, q, sz);
      u.sso[sz] = 0;
    } else {
      u.heap = new char[sz + 1];
      memcpy(u.heap, q, sz);
      u.heap[sz] = 0;
    }
  }

  string(const string &other): len(other.len) {
    if (small()) {
      memcpy(u.sso, other.u.sso, len + 1);
    } else {
      u.heap = new char[len + 1];
      memcpy(u.heap, other.u.heap, len + 1);
    }
  }

  string(string &&other): len(other.len) {
    u = other.u;
    other.len = 0;
    other.u.heap = nullptr;
  }

  string &operator=(const string &other) {
    if (this == &other)
      return *this;

    if (large())
      delete[] u.heap;
    len = other.len;
    if (small()) {
      memcpy(u.sso, other.u.sso, len + 1);
    } else {
      u.heap = new char[len + 1];
      memcpy(u.heap, other.u.heap, len + 1);
    }
    return *this;
  }

  string &operator=(string &&other) {
    if (this == &other)
      return *this;
    
    if (large())
      delete[] u.heap;
    len = other.len;
    u = other.u;
    other.len = 0;
    other.u.heap = nullptr;
    return *this;
  }

  const char *c_str() const { return small() ? u.sso : u.heap; }
  char *c_str() { return small() ? u.sso : u.heap; }

  size_t rfind(char t, size_t start = npos) const {
    start = min(start, len - 1);
    for (size_t i = start; i--; ) {
      if (c_str()[i] == t)
        return i;
    }
    return npos;
  }

  size_t find(char t, size_t start = 0) const {
    for (size_t i = start; i < len; i++) {
      if (c_str()[i] == t)
        return i;
    }
    return npos;
  }

  string substr(size_t from, size_t l = npos) const {
    size_t end = l == npos ? len : min(from + min(l, len), len);
    return string(c_str() + from, end - from);
  }

  ~string() {
    if (large())
      delete[] u.heap;
  }

  string &operator+=(const string &other) {
    return *this = *this + other;
  }

  string operator+(const string &other) const {
    string s;
    s.len = len + other.len;
    if (s.large())
      s.u.heap = new char[s.len + 1];
    memcpy(s.c_str(), c_str(), len);
    memcpy(s.c_str() + len, other.c_str(), other.len);
    s[s.len] = 0;
    return s;
  }

  void push_back(char v) {
    *this += string(&v, 1);
  }

  string join(const vector<string> &v) const {
    string result;
    for (size_t i = 0; i < v.size(); i++) {
      result += v[i];
      if (i + 1 < v.size())
        result += *this;
    }
    return result;
  }

  bool empty() const { return len == 0; }
  size_t size() const { return len; };

  char &operator[](size_t s) { return c_str()[s]; }
  char operator[](size_t s) const { return c_str()[s]; }

  bool operator==(const string &other) const { return strcmp(c_str(), other.c_str()) == 0; }
  bool operator!=(const string &other) const { return strcmp(c_str(), other.c_str()) != 0; }
  bool operator<=(const string &other) const { return strcmp(c_str(), other.c_str()) <= 0; }
  bool operator>=(const string &other) const { return strcmp(c_str(), other.c_str()) >= 0; }
  bool operator<(const string &other) const { return strcmp(c_str(), other.c_str()) < 0; }
  bool operator>(const string &other) const { return strcmp(c_str(), other.c_str()) > 0; }

  bool operator==(const char *other) const { return strcmp(c_str(), other) == 0; }
  bool operator!=(const char *other) const { return strcmp(c_str(), other) != 0; }
  bool operator<=(const char *other) const { return strcmp(c_str(), other) <= 0; }
  bool operator>=(const char *other) const { return strcmp(c_str(), other) >= 0; }
  bool operator<(const char *other) const { return strcmp(c_str(), other) < 0; }
  bool operator>(const char *other) const { return strcmp(c_str(), other) > 0; }
};

inline string operator+(const char *p, const string &q) {
  return string(p) + q;
}

struct string_view {
  const char *start = nullptr;
  size_t sz = 0;

  bool empty() const { return sz == 0; }
  size_t size() const { return sz; }
};

class split_range {
  const char* str;
  const char* delim;

public:
  split_range(const char* s, const char* d): str(s), delim(d) {}

  class iterator {
    const char *start, *end;
    const char *full_str_end;
    const char *delim;
    size_t delim_len;

    void find_next() {
      if (start >= full_str_end) {
        start = end = full_str_end;
        return;
      }

      const char *next = strstr(start, delim);
      end = next ? next : full_str_end;
    }

  public:
    iterator(const char* s, const char* end, const char* d): start(s), full_str_end(end), delim(d), delim_len(strlen(d)) {
      find_next();
    }

    iterator(const char* end): start(end), end(end), full_str_end(end), delim(nullptr), delim_len(0) {}

    string operator*() const { return string(start, end - start); }

    iterator& operator++() {
      if (start >= full_str_end)
        return *this;
      
      start = end + delim_len;
      find_next();
      return *this;
    }

    bool operator==(const iterator& other) const { return start == other.start; }
    bool operator!=(const iterator& other) const { return start != other.start; }
  };

  iterator begin() const { return iterator(str, str + strlen(str), delim); }
  iterator end() const { return iterator(str + strlen(str)); }
};

inline split_range split(const char *str, const char *delim) {
  return split_range(str, delim);
}

inline split_range split(const string &str, const char *delim) {
  return split_range(str.c_str(), delim);
}

}

namespace os::detail {

template<>
struct fnv_1a<string> {
  uint64_t operator()(const string &key) const {
    return fnv_1a<const char*>()(key.c_str());
  }
};

}

#endif
