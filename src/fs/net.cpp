#include "net.h"
#include "../proc/schedule.h"

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

void demux::push(char *buf, int len) {
  // Buffer too small.
  if ((unsigned) len < sizeof(eth::header))
    return;

  // Free the buffer on function exit.
  struct _free {
    char *buf;
    _free(char *buf): buf(buf) {}
    ~_free() { delete buf; }
  } _free(buf);

  auto ethtype = htons(((eth::header *) buf)->ethtype);
  
  // Handle Ethernet frame.
  if (ethtype == 0x800) {
    printk("recv ethernet\n");
    // TODO: ip
    return;
  }
  // Handle ARP frame.
  if (ethtype == 0x806) {
    printk("recv arp\n");
    return;
  }
  printk("kernel warning: unknown ethertype %x, packet dropped.\n", ethtype);
}

void eth::fill_header(char *p, const address dst, const address src) {
  header *h = (header *) p;
  memcpy(h->dst, dst, sizeof(address));
  memcpy(h->src, src, sizeof(address));
  h->ethtype = htons(0x800);
}

int eth::write(net_device *dev, const void *data, size_t len, const address dst, const address src, int flags) {
  auto p = ((char *) data) - sizeof(eth::header);
  eth::fill_header(p, dst, src);
  return dev->write(p, len, flags & O_NONBLOCK);
}

int arp::write(net_device *dev, const void *data, size_t len, ip::address dst, int flags) {
  // Always send to qemu user MAC address for now, no matter what `dst` is.
  (void) dst;
  return eth::write(dev, data, len, qemu_user_addr, dev->mac, flags);
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
}

int ip::write(net_device *dev, const void *data, size_t len, address src, address dst, protocol prot, int flags) {
  char *p = ((char *) data) - sizeof(ip::header);
  // Always send to 10.0.2.2 for now.
  auto arpdst = htonl(0x0a000202);
  ip::fill_header(p, dst, src, len, prot);
  return arp::write(dev, p, len, arpdst, flags);
}

static unsigned checksum_add(unsigned sum, const void *h, unsigned len) {
  auto p = (const unsigned char *) h;
  while (len > 0) {
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
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return ~sum;
}

unsigned short udp::checksum(ip::address src, ip::address dst, const void *udp, unsigned int payload_len) {
  uint32_t sum = 0;
  uint16_t udp_len = sizeof(udp::header) + payload_len;

  sum += src & 0xffff + (src >> 16);
  sum += dst & 0xffff + (dst >> 16);
  sum += htons(protocol::UDP);
  sum += htons(udp_len);
  sum = checksum_add(sum, udp, udp_len);
  
  while (sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  sum = ~sum;

  // UDP rule: checksum of zero is transmitted as all ones.
  return sum ? sum : 0xffff;
}

void udp::fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, size_t payload_len) {
  auto *h = (udp::header *) p;
  h->dstport = dstport;
  h->srcport = srcport;
  h->checksum = 0;
  h->len = payload_len + sizeof(udp::header);

  h->checksum = udp::checksum(src, dst, h, payload_len);
}

udp_socket_inode::udp_socket_inode(net_device *dev, ip::address src, unsigned short port):
  inode_impl(&sockfs, 0, 0), dev(dev), src(src), srcport(port) {
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
    auto l = min(len, (unsigned long) dg.size);
    memcpy(buf, dg.data, l);
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
    int ret = ip::write(dev, q, l + sizeof(udp::header), src, dst, protocol::UDP, flags);
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

}
