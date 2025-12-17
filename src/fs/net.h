#ifndef NET_H
#define NET_H

#include "vfs.h"

namespace os {

inline constexpr unsigned short htons(unsigned short x) {
  unsigned byte0 = x & 0xff;
  unsigned byte1 = (x >> 8) & 0xff;
  return byte1 + (byte0 << 8);
}

inline constexpr unsigned htonl(unsigned x) {
  return to_big_endian(x);
}

// Protocol numbers, from https://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml
enum protocol : unsigned char {
  HOPOPT = 0,
  ICMP = 1,
  IGMP = 2,
  IPv4 = 4,
  TCP = 6,
  UDP = 17,
  IPv6 = 41,
};

class tcb_t;
class udp_socket_inode;
struct net_device;

class demux {
  // Maps port to inode.
  os::hashmap<int, udp_socket_inode*> udps;
public:
  void push(char *buf, int len);
  void record(inode *node);
};

extern static_storage<demux> demux;

namespace eth {

extern spinlock lock;

// Ethernet address has 48 bits.
using address = char[6];
extern address src;

struct [[gnu::packed]] header {
  address dst, src;
  unsigned short ethtype;
};

void fill_header(char *p, const address dst, const address src);
inline void fill_header(char *p, const address dst) {
  fill_header(p, dst, src);
}

// This is just ff::ff::ff::ff::ff::ff
constexpr address broadcast_addr = { (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff, (char) 0xff };

int write(net_device *dev, const void *data, size_t len, const address dst, const address src, int flags);

}

namespace ip {

extern spinlock lock;

// IPv4 address. Note this is big-endian.
using address = unsigned;
extern address src;

struct header {
  // Version (4 bit) and length (4 bit).
  unsigned char ver_ihl;
  // We don't really need this.
  unsigned char dscp_epn;
  // Length of the entire package.
  unsigned short len;
  // Segmentation ID.
  unsigned short id;
  // Flags and segmentation offset.
  unsigned short flags_offset;
  // Time to live.
  unsigned char ttl;
  // The protocol at next level.
  unsigned char protocol;
  // Checksum.
  unsigned short checksum;
  // Addresses.
  address src, dst;
};

static_assert(sizeof(header) == 20);

void fill_header(char *p, address dst, address src, unsigned short len, protocol prot);
inline void fill_header(char *p, address dst, unsigned short len, protocol prot) {
  return fill_header(p, dst, src, len, prot);
}

unsigned short checksum(const void *h, unsigned len = sizeof(header));

constexpr size_t MTU = 1500;

// Assumes `data` has enough space at front for IP header.
//  [-h-|--ip payload--]
//      |---------------- `data`
int write(net_device *dev, const void *data, size_t len, address src, address dst, protocol prot, int flags);

}

namespace arp {

using address = eth::address;

// This is the MAC address that QEMU user automatically handles.
// No ARP for now; just send it there.
// (Even if we implemented ARP later, it doesn't conflict with this:
// it's just that we won't receive ARP packages from other hosts.)
constexpr address qemu_user_addr = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x57};

// MAC address of the network device.
extern address mac;

int write(net_device *dev, const void *data, size_t len, ip::address dst, int flags);

}

namespace udp {

using port = unsigned short;

struct header {
  port srcport, dstport;
  unsigned short len;
  unsigned short checksum;
};

static_assert(sizeof(header) == 8);

constexpr size_t MTU = ip::MTU - sizeof(header);

void fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, size_t payload_len);

unsigned short checksum(ip::address src, ip::address dst, const void *udp, unsigned payload_len);

}

struct /*interface*/ net_device {
  virtual int write(const void *buf, int len, bool block) = 0;
  virtual bool write_full() = 0;
  eth::address mac;
};

class udp_socket_inode : public inode_impl<udp_socket_inode> {
  net_device *dev;
  spinlock lock;

  struct datagram {
    unsigned size;
    char data[1536];
  };

  os::list<datagram> rx;
  os::vector<tcb_t*> wait;

  void on_receive(datagram &&dat);

  friend class demux;
public:
  ip::address src, dst;
  int srcport, dstport;

  FILE_INODE_DEFAULT_IMPL;

  udp_socket_inode(net_device *dev, ip::address src, unsigned short port);
  size_t read(size_t, void *buf, size_t len, int flags) override;
  size_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;
  // Sockets do not support things like `fstatat`.
  meta get_meta() override { return meta(0, 0, 0); }
  void set_meta(const inode::meta &) override {}

  void connect(ip::address addr, unsigned short port);
};

// An empty filesystem that does nothing.
class socketfs : public fs {
  inode *get() override { return nullptr; }
  void erase(inode *) override {}
  bool has_backup() override { return false; }
} extern sockfs;

}

#endif
