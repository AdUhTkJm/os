#ifndef BITMAP_H
#define BITMAP_H

#include "utility.h"
#include "../helper_meta.h"

namespace os {

template<size_t Size, typename T = unsigned> requires is_integral_v<T>
class bitmap {
public:
  using unit = T;
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
      return (elem & (1u << bitpos)) != 0;
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
      return (elem & (1u << bitpos)) != 0;
    }
    bool operator!() const {
      return !(bool) *this;
    }
  };

  reference operator[](size_t i) {
    assert(i < size);
    return reference(map[i / unit_bits], i % unit_bits);
  }

  const_reference operator[](size_t i) const {
    assert(i < size);
    return const_reference(map[i / unit_bits], i % unit_bits);
  }

  void clear(size_t begin, size_t end) {
    if (begin >= end || begin >= size)
      return;

    if (end > size)
      end = size;

    uint8_t* bytes = (uint8_t *) map;

    size_t start_byte = begin / 8, end_byte = (end - 1) / 8;
    unsigned start_bit = begin % 8, end_bit = (end - 1) % 8;

    if (start_byte == end_byte) {
      uint8_t mask = (0xff << start_bit) & (0xff >> (7 - end_bit));
      bytes[start_byte] &= ~mask;
      return;
    }

    if (start_bit != 0) {
      bytes[start_byte] &= ~(0xff << start_bit);
      start_byte++;
    }

    if (start_byte <= end_byte - 1)
      memset(bytes + start_byte, 0, end_byte - start_byte);

    if (end_bit != 7)
      bytes[end_byte] &= ~(0xff >> (7 - end_bit));
    else
      bytes[end_byte] = 0;
  }

  void set(size_t begin, size_t end) {
    if (begin >= end || begin >= size)
      return;

    if (end > size)
      end = size;

    uint8_t* bytes = (uint8_t *) map;

    size_t start_byte = begin / 8, end_byte = (end - 1) / 8;
    unsigned start_bit = begin % 8, end_bit = (end - 1) % 8;

    if (start_byte == end_byte) {
      uint8_t mask = (0xff << start_bit) & (0xff >> (7 - end_bit));
      bytes[start_byte] |= mask;
      return;
    }

    if (start_bit != 0) {
      bytes[start_byte] |= (0xff << start_bit);
      start_byte++;
    }

    if (start_byte <= end_byte - 1)
      memset(bytes + start_byte, -1, end_byte - start_byte);

    if (end_bit != 7)
      bytes[end_byte] |= (0xff >> (7 - end_bit));
    else
      bytes[end_byte] = 0xff;
  }

  void clear() { memset(map, 0, sizeof(map)); }
  unit *data() { return map; }
  const unit *data() const { return map; }
  unit word(size_t i) const { assert(i < roundup<unit_bits>(size) / unit_bits); return map[i]; }
private:
  unit map[os::roundup<unit_bits>(Size) / unit_bits];
};

}

#endif
