#include "tcp.h"
#include "../proc/schedule.h"
#include "../driver/lo/lo.h"
#include "../driver/virtio/virtio.h"
#include "../utils/log.h"

namespace os {

spinlock eth::lock;
eth::address eth::src;

spinlock ip::lock;
ip::address ip::src, ip::subnet_mask;
static_storage<os::vector<ip::address>> ip::routers, ip::dns;
static_storage<os::vector<ip::route>> ip::routes;

static_storage<class demux> demux;
socketfs sockfs;

const char *hostname() {
  static char buf[16] = "pristine-";
  sprintf(buf + 9 , "%02x", eth::src[3]);
  sprintf(buf + 11, "%02x", eth::src[4]);
  sprintf(buf + 13, "%02x", eth::src[5]);
  buf[15] = '\0';
  return buf;
}

void demux::push(const char *buf, int len) {
  // Buffer too small.
  if ((unsigned) len < sizeof(eth::header))
    return;

  eth::read(buf, len);
}

void eth::read(const char *p, size_t len) {
  auto ethtype = htons(((const eth::header *) p)->ethtype);
  
  // Handle Ethernet frame.
  if (ethtype == 0x800) {
    ip::read(p + sizeof(eth::header), len - sizeof(eth::header));
    return;
  }
  // Handle ARP frame.
  if (ethtype == 0x806) {
    printk("recv arp\n");
    return;
  }
  printk("kernel warning: unknown ethertype %x, packet dropped.\n", ethtype);
}

// Note the final 32-bit CRC is not included. It is handled and checked entirely by layer 2.
void eth::fill_header(char *p, const address dst, const address src) {
  header *h = (header *) p;
  memcpy(h->dst, dst, sizeof(address));
  memcpy(h->src, src, sizeof(address));
  h->ethtype = htons(0x800);
}

int eth::write(net_device *dev, const void *data, size_t len, const address dst, const address src, int flags, option options) {
  (void) options;

  auto p = ((char *) data) - sizeof(eth::header);
  eth::fill_header(p, dst, src);
  return dev->write(p, len + sizeof(eth::header), flags & O_NONBLOCK);
}

int arp::write(net_device *dev, const void *data, size_t len, ip::address dst, int flags, option options) {
  // For broadcast, we can completely skip ARP.
  if (options.broadcast)
    return eth::write(dev, data, len, eth::broadcast_addr, eth::src, flags, options);

  // Always send to qemu user MAC address for now, no matter what `dst` is.
  (void) dst;
  return eth::write(dev, data, len, qemu_user_addr, eth::src, flags, options);
}

void ip::read(const char *p, size_t len, int error) {
  auto header = (const ip::header *) p;
  // Verify checksum.
  if (checksum(header) != 0) {
    printk("kernel warning: bad ip checksum, dropped\n");
    return;
  }

  [[unlikely]] if (len < (header->ver_ihl & 0xf) * 4) {
    printk("ip header too short, dropped\n");
    return;
  }

  p += sizeof(ip::header);
  len -= sizeof(ip::header);

  switch (header->protocol) {
  case ICMP:
    assert(error == 0);
    icmp::read(p, len);
    break;
  case UDP:
    udp::read(p, len, error);
    break;
  case TCP:
    tcp::read(p, len, error);
    break;
  default:
    printk("kernel warning: unknown protocol %d, dropped\n", header->protocol);
  }
}

string ip::format(__big ip::address addr) {
  __little ip::address ip = htonl(addr);
  char buf[20];
  sprintf(buf, "%d.%d.%d.%d", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
  return buf;
}

optional<ip::address> ip::format(const string &addr) {
  ip::address ip = 0;
  unsigned comp = 0;
  int dots = 0;

  for (const char *p = addr.c_str(); *p; p++) {
    if (*p >= '0' && *p <= '9') {
      comp = comp * 10 + (*p - '0');
      if (comp > 255)
        return nullopt;
    } else if (*p == '.') {
      if (dots >= 3)
        return 0;

      ip = (ip << 8) | comp;
      comp = 0;
      dots++;
    } else return nullopt;
  }

  if (dots != 3)
    return nullopt;

  return (ip << 8) | comp;
}

static constexpr int popcount(unsigned x) {
  int c = 0;
  while (x) {
    x &= x - 1;
    c++;
  }
  return c;
}

ip::route *ip::lookup_route(ip::address dst) {
  route *best = nullptr;
  int maxbits = -1;
  synchronized _(ip::routes.lock);

  for (auto &r : *routes) {
    if ((dst & r.mask) == r.network) {
      int bits = popcount(r.mask);
      if (bits > maxbits) {
        best = &r;
        maxbits = bits;
      }
    }
  }
  return best;
}

void ip::fill_header(char *p, address dst, address src, unsigned short len, protocol prot) {
  static unsigned short id = 0;

  header *h = (header *) p;
  h->ver_ihl = (4 << 4) | 5;
  h->dscp_epn = 0;
  h->len = htons(len + 20);
  h->ttl = 64;
  h->protocol = prot;
  h->checksum = 0;
  h->src = src;
  h->dst = dst;
  h->flags_offset = 0;
  
  lock.acquire();
  h->id = htons(id++);
  lock.release();

  h->checksum = checksum(h);
  // The new checksum has to be zero.
  assert(checksum(h) == 0);
}

int ip::write(const void *data, size_t len, address src, address dst, protocol prot, int flags, option options) {
  char *p = ((char *) data) - sizeof(ip::header);
  ip::address arpdst;
  net_device *dev = nullptr;
  if (options.broadcast) {
    arpdst = 0xffffffff;
    // We might be setting up DHCP, in which case we don't have a routing table.
    dev = virtio::netdev();
  } else {
    auto route = lookup_route(dst);
    if (!route)
      return -ENETUNREACH;
    arpdst = route->gateway ? route->gateway : dst;
    dev = route->dev;
  }
  ip::fill_header(p, dst, src, len, prot);
  return arp::write(dev, p, len + sizeof(ip::header), arpdst, flags, options);
}

unsigned ip::checksum_add(unsigned sum, const void *h, unsigned len) {
  auto p = (const unsigned short *) h;
  while (len >= 2) {
    sum += *p++;
    len -= 2;
  }
  if (len == 1)
    sum += *(const uint8_t *) p;
  return sum;
}

unsigned short ip::checksum_fold(unsigned sum) {
  // One's complement addition ("looparound carries") is associative;
  // `sum >> 16` are "accumulated carries" across summations, and we apply them all at once.
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

unsigned short ip::checksum(const void *h, unsigned len) {
  unsigned sum = checksum_add(0, h, len);
  return checksum_fold(sum);
}

void udp::read(const char *p, size_t len, int error) {
  if (len < sizeof(udp::header))
    return;

  auto header = (const udp::header *) p;
  // If we're reading an error message from ICMP, then it's the source port that triggers the error;
  // Otherwise, this is a real message for `dstport`.
  auto port = error ? header->srcport : header->dstport;
  // Inform user program.
  if (!demux->udps.count(port)) {
    printk("kernel warning: unknown port %d, dropped\n", htons(port));
    return;
  }
  auto node = demux->udps[port];
  if (error != 0) {
    node->receive(error);
    return;
  }
  
  // To get the IP address, we must examine the IP header.
  auto ip_header = (const ip::header *) (p - sizeof(ip::header));
  node->recv = ip_header->src;
  node->recvport = header->srcport;
  udp_socket_inode::datagram g(p + sizeof(udp::header), len - sizeof(udp::header));
  node->receive(os::move(g));
}

unsigned short udp::checksum(ip::address src, ip::address dst, const void *udp, unsigned int payload_len) {
  unsigned sum = 0;
  uint16_t udp_len = sizeof(udp::header) + payload_len;

  sum += (src & 0xffff) + (src >> 16);
  sum += (dst & 0xffff) + (dst >> 16);
  sum += htons(protocol::UDP);
  sum += htons(udp_len);
  sum = ip::checksum_add(sum, udp, udp_len);
  sum = ip::checksum_fold(sum);

  // UDP rule: checksum of zero is transmitted as all ones.
  // This is because zero is interpreted as not using checksums.
  return sum ? sum : 0xffff;
}

void icmp::read(const char *p, size_t len) {
  if (len < sizeof(icmp::header))
    return;

  auto header = (const icmp::header *) p;
  p += sizeof(icmp::header);
  len -= sizeof(icmp::header);
  int error = 0;
  switch (header->type) {
  case 0:
  case 8:
    printk("icmp: echo headers, not implemented yet\n");
    return;
  case 3: // Destination unreachable.
    switch (header->code) {
    case 3: error = EHOSTUNREACH; break; // Dest port unreachable
    case 1: error = EHOSTUNREACH; break; // Dest host unreachable
    case 0: error = ENETUNREACH;  break; // Dest net  unreachable
    default: error = ENETUNREACH;
    }
    break;
  case 11: // Timeout.
    error = ETIMEDOUT;
    break;
  default:
    printk("icmp: unknown header type %d, dropped\n", header->type);
    return;
  }
  ip::read(p, len, error);
}

void udp::fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, size_t payload_len, bool check) {
  auto *h = (udp::header *) p;
  h->dstport = dstport;
  h->srcport = srcport;
  h->checksum = 0;
  h->len = htons(payload_len + sizeof(udp::header));
  h->checksum = check ? udp::checksum(src, dst, h, payload_len) : 0;
}

udp_socket_inode::udp_socket_inode():
  inode_impl(&sockfs, 0, 0, 0666, Socket), src(ip::src), srcport(0) {
}

udp_socket_inode::~udp_socket_inode() {
  if (srcport != 0)
    demux->udps.erase(srcport);
}

void udp_socket_inode::receive(datagram &&data) {
  rxlock.acquire();
  rx.push_back(os::move(data));
  rxlock.release();

  readwait.wake_all();
}

void udp_socket_inode::receive(int error) {
  rxlock.acquire();
  rxerr = error;
  rxlock.release();

  readwait.wake_all();
}

ssize_t udp_socket_inode::read(size_t, void *buf, size_t len, int flags) {
  bool block = !(flags & O_NONBLOCK);

  wait_entry entry;
  rxlock.acquire();
  for (;;) {
    if (rxerr) {
      int err = rxerr;
      rxerr = 0;
      rxlock.release();
      return err;
    }

    if (!rx.empty()) {
      datagram dg = os::move(rx.front());
      rx.pop_front();
      rxlock.release();

      auto l = min(len, (unsigned long) dg.size());
      memcpy(buf, dg.c_str(), l);
      return l;
    }

    if (!block) {
      rxlock.release();
      return -EAGAIN;
    }

    hangon(readwait, rxlock, entry);
  }
}

ssize_t udp_socket_inode::write(size_t, const void *buf, size_t len, int flags) {
  // Allocate a port when there's none.
  if (!srcport) {
    if (auto ret = bind(src, 0); ret < 0)
      return ret;
  }

  size_t off = 0;
  const char *p = (const char *) buf;
  // Total header length.
  constexpr size_t total = sizeof(eth::header) + sizeof(ip::header) + sizeof(udp::header);
  char data[udp::MTU + total];

  for (; off < len; off += udp::MTU) {
    auto l = min(len - off, udp::MTU);
    memcpy(data + total, p + off, l);

    auto q = data + total - sizeof(udp::header);
    udp::fill_header(q, src, dst, srcport, dstport, len, options.checksum);
    int ret = ip::write(q, l + sizeof(udp::header), src, dst, protocol::UDP, flags, options);
    if (ret < 0)
      return off ? off : ret;
  }
  return off;
}

udp::port udp_socket_inode::allocate() {
  constexpr int max = 61000, min = 32768;
  static int port = min;
  static spinlock lock;

  synchronized _(lock);
  int end = port;
  do {
    if (++port == max)
      port = min;
    if (!demux->udps.count(port))
      return port;
  } while (port != end);
  return 0;
}

short udp_socket_inode::poll(unsigned short events) {
  short result = 0;
  if (events & POLLIN && rx.size())
    result |= POLLIN;
  // Perhaps delegate to lower layers?
  if (events & POLLOUT)
    result |= POLLOUT;
  return result;
}

int udp_socket_inode::bind(ip::address src, udp::port port) {
  if (port == 0) {
    if (port = htons(allocate()); !port)
      return -EADDRINUSE;
  }
  if (demux->udps.count(port))
    return -EADDRINUSE;
  
  this->src = src;
  if (srcport != 0)
    demux->udps.erase(srcport);
  demux->udps.insert(srcport = port, this);
  return 0;
}

int udp_socket_inode::connect(ip::address addr, udp::port port) {
  // No special thing needed. UDP doesn't establish connection as TCP does.
  dst = addr;
  dstport = port;
  return 0;
}

void udp_socket_inode::wake_read() {
  readwait.wake_all();
}

void udp_socket_inode::prepare_read_wait(wait_entry &entry) {
  readwait.prepare(entry);
}

void udp_socket_inode::finish_read_wait(wait_entry &entry) {
  readwait.finish(entry);
}

void dhcp::fill_option(unsigned char *&dst, unsigned char type, const char *src, unsigned char len) {
  dst[0] = type;
  dst[1] = len;
  memcpy(dst + 2, src, len);
  dst = dst + 2 + len;
}

void dhcp::daemon() {
  ip::routers.construct();
  ip::dns.construct();
  demux.construct();

  // The IP addresses stay the same after conversion to big-endian, so we omit the conversion here.
  auto sock = new udp_socket_inode();
  sock->bind(ip::src, htons(68));
  sock->connect(0xffffffff, htons(67));
discover:
  alignas(4) unsigned char payload[300] = { 0x01, 0x01, 0x06, 0x00 };
  unsigned xid = rand(), netxid = htonl(xid), cookie = htonl(0x63825363);
  
  memcpy(payload + 4, &netxid, 4);
  memcpy(payload + 28, eth::src, sizeof(eth::address));
  memcpy(payload + 236, &cookie, 4);
  
  // Fill in options.
  auto opt = payload + 240;

  // DHCP protocol type.
  char discover = /*DHCPDISCOVER*/ 1;
  fill_option(opt, 53, &discover, 1);

  // Queries for the DHCP server.
  // For meanings, see https://en.wikipedia.org/wiki/Dynamic_Host_Configuration_Protocol#Options
  char buf[] = { /*subnet mask*/ 1, /*router*/ 3, /*dns server*/ 6 };
  fill_option(opt, 55, buf, sizeof(buf));

  // Notify server about our ethernet address.
  fill_option(opt, 61, eth::src, sizeof(eth::address));

  // Tell server my hostname.
  auto name = hostname();
  fill_option(opt, 12, name, strlen(name));
  
  // Add end marker.
  *opt++ = 0xff;

  // Write it.
  sock->options.broadcast = true;
  sock->write(0, payload, opt - payload, 0);
  printk("dhcp sent\n");

  // Read.
  alignas(4) unsigned char recv[400];
  int len = sock->read(0, recv, sizeof(recv), 0);
  printk("dhcp received\n");
  if (len < 0) {
    printk("dhcp: sock error: %d\n");
    goto discover;
  }

  // Extract information from it.
  unsigned recvcookie;
  memcpy(&recvcookie, recv + 236, 4);
  bool valid = len >= 240 && recvcookie == htonl(0x63825363);
  if (!valid)
    goto discover;

  ip::address src, subnet_mask, siaddr;
  os::vector<ip::address> routers, dns;
  memcpy(&src, recv + 16, 4);
  memcpy(&siaddr, recv + 20, 4);

  for (auto opt = recv + 240; opt - recv < 400 && *opt != 0xff; ) {
    auto type = *opt++;
    // This is just padding.
    if (type == 0)
      continue;

    auto len = *opt++;
    switch (type) {
    case 1:
      memcpy(&subnet_mask, opt, 4);
      break;

    case 3:
      for (unsigned i = 0; i < len; i += 4) {
        ip::address router;
        memcpy(&router, opt, 4);
        routers.push_back(router);
      }
      break;

    case 6:
      for (unsigned i = 0; i < len; i += 4) {
        ip::address addr;
        memcpy(&addr, opt, 4);
        dns.push_back(addr);
      }
      break;
    
    case 53:
      if (*opt != 2)
        valid = false;
      break;
    
    default:
      printk("dhcp: warning: unknown option %d\n", type);
      break;
    }
    opt += len;
  }

  if (valid) {
    synchronized _(ip::lock);
    ip::src = src;
    ip::subnet_mask = subnet_mask;
    *ip::routers = os::move(routers);
    *ip::dns = os::move(dns);
  } else goto discover;

  // Now send a request message, still broadcast.
  // The front part of payload is largely reusable, so we don't clear it.
request:
  memset(payload + 240, 0, sizeof(payload) - 240);
  memset(recv, 0, sizeof(recv));

  memcpy(payload + 20, &siaddr, 4);

  // Fill in options and send it. The broadcast flag is already true.
  char request = 3;
  opt = payload + 240;
  fill_option(opt, 53, &request, 1);
  fill_option(opt, 50, (const char*) &src, sizeof(ip::address));
  fill_option(opt, 54, (const char*) &siaddr, sizeof(ip::address));
  *opt++ = 0xff;
  sock->write(0, payload, opt - payload, 0);

  // Look at the final acknowledge packet.
  len = sock->read(0, recv, sizeof(recv), 0);
  if (len < 0) {
    printk("dhcp: sock error: %d\n");
    goto discover;
  }

  memcpy(&recvcookie, recv + 236, 4);
  valid = len >= 240 && recvcookie == htonl(0x63825363);
  if (!valid)
    goto request;
  
  unsigned lease_time = 0;
  for (auto opt = recv + 240; opt - recv < 400 && *opt != 0xff; ) {
    auto type = *opt++;
    auto len = *opt++;
    switch (type) {
    case 53:
      if (*opt != 4)
        valid = false;
      break;

    case 51:
      memcpy(&lease_time, opt, 4);
      lease_time = htonl(lease_time);

    default: 
      ; // Ignored. The information is already included.
    }
    opt += len;
  }

  printk("dhcp: lease time: %u\n", lease_time);

  // Fill in route table.
  {
    assert(ip::routes.valid());
    synchronized _(ip::routes.lock);

    ip::routes->clear();
    // 127.0.0.1/8
    ip::routes->push_back({
      .network = htonl(0x7f000001),
      .mask = 0xffffff00,
      .gateway = 0,
      .dev = &lo::localhost
    });

    // src/netmask (obtained from DHCP)
    ip::routes->push_back({
      .network = ip::src,
      .mask = ip::subnet_mask,
      .gateway = 0,
      .dev = virtio::netdev()
    });

    // 0.0.0.0/0 -> routers[0]
    ip::routes->push_back({
      .network = 0,
      .mask = 0,
      .gateway = (*ip::routers)[0],
      .dev = virtio::netdev()
    });
  }

  // Configure DNS. In the buildroot rootfs, we expect it to be at /tmp/resolv.conf.
  auto tcb = active();
  auto pcb = tcb->pcb;
  auto fd = pcb->open_file("/tmp/resolv.conf", O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    printk("fd: %d\n", fd), panic("dhcp: cannot create /tmp/resolv.conf");

  // Write the file like: `nameserver 10.0.2.2\n`
  auto file = pcb->ftbl->at(fd);
  string msg = "nameserver ";
  assert(ip::dns->size());
  msg += ip::format((*ip::dns)[0]);
  msg += "\n";
  file->write(msg.c_str(), msg.size());
  pcb->close_file(fd);
  
  tcb->sleep(lease_time * 500'000'000ul /*ns*/);
  goto discover;
}

}
