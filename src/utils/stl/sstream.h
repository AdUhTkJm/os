#ifndef SSTREAM_H
#define SSTREAM_H

#include "vector.h"
#include "string.h"

namespace os {

class sstream {
  vector<char> buf;
public:
  sstream &operator<<(const char *p) {
    while (*p)
      buf.push_back(*p++);
    return *this;
  }

  sstream &operator<<(const string &str) {
    buf.resize(buf.size() + str.size());
    memcpy(buf.data() + buf.size(), str.c_str(), str.size());
    return *this;
  }

  template<class T> requires (is_integral_v<T> && !is_same_v<T, char>)
  sstream &operator<<(T v) {
    char buf[40];
    itoa(v, buf, 10);
    return *this << buf;
  }

  sstream &operator<<(char v) {
    buf.push_back(v);
    return *this;
  }

  char *data() { return buf.data(); }
  const char *data() const { return buf.data(); }

  size_t size() const { return buf.size(); }
};

}

#endif
