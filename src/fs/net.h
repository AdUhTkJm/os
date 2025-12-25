#ifndef NET_H
#define NET_H

#include "vfs.h"
#include "../lock/mutex.h"

// These are just remainders for myself. They don't enforce anything.
#define __little
#define __big

// From <socket.h>
#define SOL_SOCKET	1

namespace os {

struct option {
  bool broadcast = false;
  bool checksum = true;
};

inline constexpr unsigned short htons(unsigned short x) {
  unsigned byte0 = x & 0xff;
  unsigned byte1 = (x >> 8) & 0xff;
  return byte1 + (byte0 << 8);
}

inline constexpr unsigned htonl(unsigned x) {
  return to_big_endian(x);
}

inline constexpr unsigned short ntohs(unsigned short x) { return htons(x); }
inline constexpr unsigned ntohl(unsigned x) { return htonl(x); }

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

struct tcb_t;
class udp_socket_inode;
class tcp_socket_inode;
struct net_device;

class demux {
public:
  // Maps port to inode.
  os::hashmap<__big unsigned short, udp_socket_inode*> udps;
  os::hashmap<__big unsigned short, tcp_socket_inode*> tcps;
  void push(char *buf, int len);
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

int write(net_device *dev, const void *data, size_t len, const address dst, const address src, int flags, option options);
void read(const char *p, size_t len);

}

namespace ip {

extern spinlock lock;

// IPv4 address. Note this is big-endian.
using address = unsigned;
extern __big address src;
extern __big address subnet_mask;
extern static_storage<os::vector<__big address>> routers, dns;

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

struct pseudo_header {
  ip::address src, dst;
  unsigned char zero;
  unsigned char prot;
  unsigned short len;
};

static_assert(sizeof(header) == 20);

void fill_header(char *p, address dst, address src, unsigned short len, protocol prot);
inline void fill_header(char *p, address dst, unsigned short len, protocol prot) {
  return fill_header(p, dst, src, len, prot);
}

unsigned checksum_add(unsigned sum, const void *h, unsigned len);
unsigned short checksum_fold(unsigned sum);
unsigned short checksum(const void *h, unsigned len = sizeof(header));

constexpr size_t MTU = 1500;

// Assumes `data` has enough space at front for IP header.
//  [-h-|--ip payload--]
//      |---------------- `data`
int write(const void *data, size_t len, address src, address dst, protocol prot, int flags, option options);
void read(const char *p, size_t len, int error = 0);

string format(__big ip::address addr);
optional<ip::address> format(const string &addr);

struct route {
  ip::address network;
  ip::address mask;
  ip::address gateway;
  net_device *dev;
};

extern static_storage<vector<route>> routes;
route *lookup_route(ip::address dst);

}

namespace icmp {

struct header {
  unsigned char  type;
  unsigned char  code;
  unsigned short checksum;
  unsigned       rest;
};

void read(const char *p, size_t len);

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

int write(net_device *dev, const void *data, size_t len, ip::address dst, int flags, option options);

}

namespace udp {

using port = __big unsigned short;

struct header {
  port srcport, dstport;
  unsigned short len;
  unsigned short checksum;
};

static_assert(sizeof(header) == 8);

constexpr size_t MTU = ip::MTU - sizeof(header);

void fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, size_t payload_len, bool check);

unsigned short checksum(ip::address src, ip::address dst, const void *udp, unsigned payload_len);
void read(const char *p, size_t len, int error = 0);

}

namespace dhcp {

void fill_option(unsigned char *&dst, unsigned char type, const char *src, unsigned char len);

// Runs DHCP and updates IP address.
void daemon();

}

struct /*interface*/ net_device {
  virtual int write(const void *buf, int len, bool block) = 0;
  virtual bool write_full() = 0;
  virtual void wake_write() = 0;
};

class udp_socket_inode : public inode_impl<udp_socket_inode> {
  spinlock rxlock;
  wait_queue readwait;

  // We just need a byte stream with length.
  // Moreover, string gives us move semantics, so better than a plain struct.
  using datagram = string;

  os::list<datagram> rx;
  int rxerr = 0;

  void receive(datagram &&dat);
  void receive(int error);

  // Allocates an unused port. This is required behaviour when no bind() is called.
  static udp::port allocate();

  friend class demux;
  friend void udp::read(const char *p, size_t len, int error);
public:
  ip::address src, dst;
  int srcport = 0, dstport;
  option options;

  FILE_INODE_DEFAULT_IMPL;

  udp_socket_inode();
  ~udp_socket_inode();
  ssize_t read(size_t, void *buf, size_t len, int flags) override;
  ssize_t write(size_t, const void*, size_t, int flags) override;
  short poll(unsigned short) override;
  void wake_read() override;
  void wake_write() override;
  // Sockets do not support things like `fstatat`.
  meta get_meta() override { return meta(0, 0, 0); }
  void set_meta(const inode::meta &) override {}

  int bind(ip::address src, udp::port port);
  int connect(ip::address addr, udp::port port);
};

class unix_socket_inode : public inode_impl<unix_socket_inode> {
  file *f;
public:
  unix_socket_inode();
  int bind(const string &path);
  int connect(const string &path);
};

// An empty filesystem that does nothing.
class socketfs : public fs {
  inode *get() override { return nullptr; }
  void erase(inode *) override {}
  bool has_backup() override { return false; }
} extern sockfs;

const char *hostname();

}

#endif
