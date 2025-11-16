#ifndef BITMAP_H
#define BITMAP_H

#include "utility.h"

namespace os {

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

}

#endif
