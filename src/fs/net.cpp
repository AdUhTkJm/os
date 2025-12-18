#include "net.h"
#include "../proc/schedule.h"
#include "../driver/virtio/virtio.h"
#include "../utils/log.h"

namespace os {

spinlock eth::lock;
eth::address eth::src;

spinlock ip::lock;
ip::address ip::src;

static_storage<class demux> demux;
socketfs sockfs;

void demux::record(inode *node) {
  if (auto udp = dyn_cast<udp_socket_inode>(node)) {
    udps.insert(udp->srcport, udp);
    return;
  }

  panic("demux::record: unknown node type");
}

const char *hostname() {
  static char buf[16] = "pristine-";
  sprintf(buf + 9 , "%02x", eth::src[3]);
  sprintf(buf + 11, "%02x", eth::src[4]);
  sprintf(buf + 13, "%02x", eth::src[5]);
  buf[15] = '\0';
  return buf;
}

void demux::push(char *buf, int len) {
  // Buffer too small.
  if ((unsigned) len < sizeof(eth::header))
    return;

  eth::read(buf, len);
  delete[] buf;
}

void eth::read(const char *p, size_t len) {
  auto ethtype = htons(((eth::header *) p)->ethtype);
  
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

void ip::read(const char *p, size_t len) {
  auto header = (ip::header *) p;
  switch (header->protocol) {
  case UDP:
    udp::read(p + sizeof(ip::header), len - sizeof(ip::header));
    break;
  default:
    printk("kernel warning: unknown protocol %d, dropped\n", header->protocol);
  }
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

  h->checksum = htons(checksum(h));
  // The new checksum has to be zero.
  assert(checksum(h) == 0);
}

int ip::write(net_device *dev, const void *data, size_t len, address src, address dst, protocol prot, int flags, option options) {
  char *p = ((char *) data) - sizeof(ip::header);
  ip::address arpdst;
  if (!options.broadcast) {
    // Always send to 10.0.2.2 for now. We might need to query route table.
    arpdst = htonl(0x0a000202);
  } else arpdst = 0xffffffff;
  ip::fill_header(p, dst, src, len, prot);
  return arp::write(dev, p, len + sizeof(ip::header), arpdst, flags, options);
}

static unsigned checksum_add(unsigned sum, const void *h, unsigned len) {
  auto p = (const unsigned char *) h;
  while (len >= 2) {
    sum += ((p[0] << 8) | p[1]);
    p += 2;
    len -= 2;
  }
  if (len == 1)
    sum += p[0] << 8;
  return sum;
}

unsigned short ip::checksum(const void *h, unsigned len) {
  unsigned sum = checksum_add(0, h, len);

  // One's complement addition ("looparound carries") is associative;
  // `sum >> 16` are "accumulated carries" across summations, and we apply them all at once.
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

void udp::read(const char *p, size_t len) {
  auto header = (udp::header *) p;
  auto port = header->dstport;
  printk("udp read\n");
  // Inform user program.
  if (!demux->udps.count(port)) {
    printk("kernel warning: unknown port %d, dropped\n", port);
    return;
  }
  auto node = demux->udps[port];
  udp_socket_inode::datagram g(p + sizeof(udp::header), len - sizeof(udp::header));
  node->on_receive(os::move(g));
}

unsigned short udp::checksum(ip::address src, ip::address dst, const void *udp, unsigned int payload_len) {
  unsigned sum = 0;
  uint16_t udp_len = sizeof(udp::header) + payload_len;

  sum += (src & 0xffff) + (src >> 16);
  sum += (dst & 0xffff) + (dst >> 16);
  sum += protocol::UDP;
  sum += htons(udp_len);
  sum = checksum_add(sum, udp, udp_len);
  
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  sum = ~sum;

  // UDP rule: checksum of zero is transmitted as all ones.
  // This is because zero is interpreted as not using checksums.
  return sum ? sum : 0xffff;
}

void udp::fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, size_t payload_len) {
  auto *h = (udp::header *) p;
  h->dstport = dstport;
  h->srcport = srcport;
  h->checksum = 0;
  h->len = htons(payload_len + sizeof(udp::header));

  // We skip UDP checksum for now.
  // It is still buggy, and skipping checksum is entirely standard-compliant.
  // Hopefully it will also be faster.
  (void) src; (void) dst;
  // h->checksum = htons(udp::checksum(src, dst, h, payload_len));
}

udp_socket_inode::udp_socket_inode(net_device *dev, ip::address src, unsigned short port):
  inode_impl(&sockfs, 0, 0, 0666, Socket), dev(dev), src(src), srcport(port) {
  demux->record(this);
}

void udp_socket_inode::on_receive(datagram &&data) {
  synchronized _(lock);
  rx.push_back(os::move(data));
  if (wait.size()) {
    lock.release();
    scheduler.wakeup_all(lock, wait);
  }
}

size_t udp_socket_inode::read(size_t, void *buf, size_t len, int flags) {
  bool block = !(flags & O_NONBLOCK);
  tcb_t *tcb = active();

  for (;;) {
    synchronized _(lock);

    if (rx.empty()) {
      if (!block)
        return -EAGAIN;
      wait.push_back(tcb);
      if (suspend() != 0)
        return -EINTR;
    
      continue;
    }

    const datagram &dg = rx.front();
    auto l = min(len, (unsigned long) dg.size());
    memcpy(buf, dg.c_str(), l);
    rx.pop_front();
    return l;
  }
}

size_t udp_socket_inode::write(size_t, const void *buf, size_t len, int flags) {
  size_t off = 0;
  const char *p = (const char *) buf;
  // Total header length.
  constexpr size_t total = sizeof(eth::header) + sizeof(ip::header) + sizeof(udp::header);
  char data[udp::MTU + total];

  for (; off < len; off += udp::MTU) {
    auto l = min(len - off, udp::MTU);
    memcpy(data + total, p + off, l);

    auto q = data + total - sizeof(udp::header);
    udp::fill_header(q, src, dst, srcport, dstport, len);
    int ret = ip::write(dev, q, l + sizeof(udp::header), src, dst, protocol::UDP, flags, options);
    if (ret < 0)
      return off ? off : ret;
  }
  return off;
}

short udp_socket_inode::poll(unsigned short events) {
  synchronized _(lock);
  short result = 0;
  if (events & POLLIN && rx.size())
    result |= POLLIN;
  if (events & POLLOUT && !dev->write_full())
    result |= POLLOUT;
  return result;
}

int udp_socket_inode::bind(ip::address src, udp::port port) {
  if (demux->udps.count(src)) {
    // Failed.
    return -EBUSY;
  }
  this->src = src;
  srcport = port;
  return 0;
}

int udp_socket_inode::connect(ip::address addr, udp::port port) {
  // No special thing needed. UDP doesn't establish connection as TCP does.
  dst = addr;
  dstport = port;
  return 0;
}

void udp_socket_inode::wake_read() {
  scheduler.wakeup_all(lock, wait);
}

void udp_socket_inode::wake_write() {
  dev->wake_write();
}

void dhcp::fill_option(unsigned char *&dst, unsigned char type, const char *src, unsigned char len) {
  dst[0] = type;
  dst[1] = len;
  memcpy(dst + 2, src, len);
  dst = dst + 2 + len;
}

void dhcp::daemon() {
  // The IP addresses stay the same after conversion to big-endian, so we omit the conversion here.
  auto sock = new udp_socket_inode(virtio::netdev(), 0, htons(68));
  sock->connect(0xffffffff, htons(67));

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
  char buf[] = { /*subnet mask*/ 1, /*router*/ 3, /*dns server*/ 6, /*ntp server*/ 42 };
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
  // Read.
  unsigned char recv[300];
  unsigned len = sock->read(0, recv, sizeof(recv), 0);
  hexdump(recv, len);
  for (;;) __asm__ volatile("wfi");
}

}
