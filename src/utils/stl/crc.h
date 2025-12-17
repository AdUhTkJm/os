#ifndef CRC_H
#define CRC_H

namespace os {

template<unsigned Poly>
class crc32c {
private:
  // TODO: Maybe make this constexpr as well/
  unsigned table[256];
  static constexpr unsigned bitswap(unsigned v) {
    unsigned r = 0;
    for (int i = 0; i < 32; i++)
      r = (r << 1) | ((v >> i) & 1);
    return r;
  }

  // We mirror the polynomial, reversing bit order.
  // In standard CRC, we process from MSB; with mirroring, we can (natively) process from LSB.
  static constexpr unsigned POLY = bitswap(Poly);

public:
  crc32c() {
    for (unsigned i = 0; i < 256; i++) {
      unsigned res = i;
      for (int j = 0; j < 8; j++)
        res = res & 1 ? (res >> 1) ^ POLY : res >> 1;
      table[i] = res;
    }
  }

  unsigned operator()(const void *data, unsigned long len, unsigned crc = 0xFFFFFFFF) const {
    auto p = (const unsigned char *) data;
    while (len--)
      crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
  }
};

}

#endif
