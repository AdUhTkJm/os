#ifndef STRING_H
#define STRING_H

#include "utility.h"
#include "hash.h"

namespace os {

class string {
  char *p;
  size_t len;
public:
  string(): string("") {}
  /* implicit */ string(const char *q): p((char *) vmalloc(strlen(q) + 1)), len(strlen(q)) {
    strcpy(p, q);
  }
  string(const char *q, size_t sz): p((char *) vmalloc(sz + 1)), len(sz) {
    memcpy(p, q, sz);
    p[sz] = 0;
  }
  string(const string &other): p((char *) vmalloc(other.len + 1)), len(other.len) {
    strcpy(p, other.p);
  }
  string(string &&other): p(other.p), len(other.len) {
    other.p = nullptr;
    other.len = 0;
  }

  string &operator=(const string &other) {
    if (this == &other)
      return *this;

    vfree(p);
    p = (char *) vmalloc(other.len + 1);
    strcpy(p, other.p);
    len = other.len;
    return *this;
  }

  string &operator=(string &&other) {
    vfree(p);
    p = other.p; other.p = nullptr;
    len = other.len;
    return *this;
  }

  ~string() { vfree(p); }

  size_t size() const { return len; };
  const char *c_str() const { return p; }
  char *c_str() { return p; }
  void dump() { printk("len = %ld, content = %s\n", len, p); }

  bool operator==(const string &other) const { return strcmp(p, other.p) == 0; }
  bool operator!=(const string &other) const { return strcmp(p, other.p) != 0; }
  bool operator<=(const string &other) const { return strcmp(p, other.p) <= 0; }
  bool operator>=(const string &other) const { return strcmp(p, other.p) >= 0; }
  bool operator<(const string &other) const { return strcmp(p, other.p) < 0; }
  bool operator>(const string &other) const { return strcmp(p, other.p) > 0; }

  bool operator==(const char *other) const { return strcmp(p, other) == 0; }
  bool operator!=(const char *other) const { return strcmp(p, other) != 0; }
  bool operator<=(const char *other) const { return strcmp(p, other) <= 0; }
  bool operator>=(const char *other) const { return strcmp(p, other) >= 0; }
  bool operator<(const char *other) const { return strcmp(p, other) < 0; }
  bool operator>(const char *other) const { return strcmp(p, other) > 0; }
};

struct string_view {
  const char *start = nullptr;
  size_t sz = 0;

  // This allocates new memory by vmalloc, and must be freed.

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
