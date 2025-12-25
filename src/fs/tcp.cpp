#include "tcp.h"
#include "../proc/schedule.h"

namespace os {

void tcp::fill_header(char *p, ip::address src, ip::address dst, port srcport, port dstport, unsigned seq, unsigned ack, unsigned char flags, unsigned short window, unsigned short payload_len) {
  auto header = (tcp::header *) p;
  header->srcport = srcport;
  header->dstport = dstport;
  header->seq = seq;
  header->ack = ack;
  header->offset = 5; // 20 byte header
  header->flags = flags;
  header->window = window;
  header->__resv = 0;
  header->urgent_ptr = 0;
  header->checksum = 0;

  // Construct a pseudo-header.
  assert(payload_len < 65515);
  ip::pseudo_header ph {
    .src = src, .dst = dst,
    .zero = 0, .prot = protocol::TCP,
    .len = htons(20 + payload_len)
  };

  unsigned check = 0;
  check = ip::checksum_add(check, &ph, sizeof(ip::pseudo_header));
  check = ip::checksum_add(check, p, 20 + payload_len);
  header->checksum = ip::checksum_fold(check);
}

void tcp::read(const char *p, size_t len, int error) {
  if (len < sizeof(tcp::header))
    return;

  auto header = (const tcp::header *) p;
  // If we're reading an error message from ICMP, then it's the source port that triggers the error;
  // Otherwise, this is a real message for `dstport`.
  auto port = error ? header->srcport : header->dstport;
  // Inform user program.
  if (!demux->tcps.count(port)) {
    printk("kernel warning: unknown port %d, dropped\n", htons(port));
    return;
  }
  auto node = demux->tcps[port];
  if (error != 0)
    node->receive(error);

  // We don't strip header here, unlike UDP; we need its info inside the inode.
  tcp_socket_inode::packet g(p, len);
  node->receive(os::move(g));
}

// Avoid `mod` on every iteration step.
void tcp_socket_inode::buffer::write(const void *buf, unsigned len) {
  auto src = (const char *) buf;
  unsigned chunk = min((unsigned) len, cap - tail);
  memcpy(data + tail, src, chunk);
  
  if (len > chunk) {
    memcpy(data, src + chunk, len - chunk);
    tail = len - chunk;
  } else 
    tail += len;
  
  [[unlikely]] if (tail == cap)
    tail = 0;
  size += len;
}

void tcp_socket_inode::buffer::consume(void *buf, unsigned len) {
  auto dst = (char *) buf;
  unsigned chunk = min((unsigned) len, cap - head);
  memcpy(dst, data + head, chunk);
  
  if (len > chunk) {
    memcpy(dst + chunk, data, len - chunk);
    head = len - chunk;
  } else
    head += len;
  
  [[unlikely]] if (head == cap)
    head = 0;
  size -= len;
}

void tcp_socket_inode::buffer::peek(void *buf, unsigned len) const {
  auto dst = (char *) buf;
  unsigned chunk = min((unsigned) len, cap - head);
  memcpy(dst, data + head, chunk);
  if (len > chunk)
    memcpy(dst + chunk, data, len - chunk);
}

tcp::port tcp_socket_inode::allocate() {
  constexpr int max = 65536, min = 49152;
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

tcp_socket_inode::tcp_socket_inode():
  inode_impl(&sockfs, 0, 0, 0666, Socket), state(tcp::state::CLOSED) {
  rxbuf.data = new char[rxbuf.cap = 65535];
  txbuf.data = new char[txbuf.cap = 65535];
}

tcp_socket_inode::~tcp_socket_inode() {
  delete[] rxbuf.data;
  delete[] txbuf.data;
}

int tcp_socket_inode::bind(ip::address addr, tcp::port port) {
  if (port == 0) {
    if (port = htons(allocate()); !port)
      return -EADDRINUSE;
  }
  if (demux->tcps.count(port))
    return -EADDRINUSE;

  src = addr;
  state = tcp::state::BOUND;
  
  if (srcport != 0)
    demux->tcps.erase(srcport);
  demux->tcps.insert(srcport = port, this);
  return 0;
}

void tcp_socket_inode::receive(packet &&data) {
  printk("receive:\n");
  hexdump(data.c_str(), data.size());
  lock.acquire();
  
  if (state != tcp::ESTABLISHED)
    recv = os::move(data);
  else
    consume(data.c_str(), data.size());

  lock.release();

  readwait.wake_all();
}

void tcp_socket_inode::receive(int error) {
  lock.acquire();
  rxerr = error;
  lock.release();

  readwait.wake_all();
}

ssize_t tcp_socket_inode::send(const buffer &buf, size_t len, unsigned char flags) {
  // TCP cannot transmit a message this long.
  if (len > 65515)
    return -EINVAL;
  if (state != tcp::ESTABLISHED)
    return -ENOTCONN;

  // Create a large enough buffer.
  unique_ptr<char[]> buffer(new char[len + total]);
  char *p = buffer.get() + total;
  buf.peek(p, len);
  p -= sizeof(tcp::header);
  
  tcp::fill_header(p, src, dst, srcport, dstport, htonl(seq), htonl(ack), flags, htons(rxwindow), 0);
  ip::write(p, len + sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
  seq += len;
  return len;
}

void tcp_socket_inode::consume(const char *p, size_t len) {
  // Verify packet.
  const auto *header = (const tcp::header *) p;
  // Calculate payload length.
  len -= header->offset * 4;
  p += header->offset * 4;
  if (len > rxbuf.size) {
    // The packet should be no longer than advertised window; drop this packet.
    printk("tcp: warning: packet malformed, dropped\n");
    return;
  }
  
  lock.acquire();
  if (header->flags & tcp::FIN) {
    fin = true;
    state = tcp::state::FIN_WAIT_1;
    printk("tcp: TODO: fin received\n");
    lock.release();
    return;
  }

  // We received in-order data.
  if (header->seq == ack) {
    rxbuf.write(p, len);
    ack += len;
    // Try to consume out-of-order buffer.
    while (true) {
      auto *str = ooo.find(ack); 
      if (!str)
        break;

      rxbuf.write(str->c_str(), str->size());
      ack += str->size();
      ooo.erase(seq); 
    }
  }

  // We received out-of-order data.
  else if (header->seq > ack)
    ooo.insert(header->seq, string(p, len));

  // We can ignore duplicate data when header->seq < ack.
  lock.release();

  // Send an ACK.
  char ackbuf[total];
  char *q = ackbuf + total - sizeof(tcp::header);
  tcp::fill_header(q, src, dst, srcport, dstport, seq, ack, tcp::ACK, 0, 0);
}

ssize_t tcp_socket_inode::read(size_t offset, void *buf, size_t len, int flags) {
  (void) offset;
  bool nonblock = flags & O_NONBLOCK;
  if (len == 0)
    return -EINVAL;

  wait_entry entry;
  lock.acquire();
  while (rxbuf.size == 0 && !fin) {
    if (nonblock) {
      lock.release();
      return -EAGAIN;
    }

    readwait.prepare(entry);
    lock.release();

    if (suspend() != 0)
      return -EINTR;

    lock.acquire();
    readwait.finish(entry);

    if (rxerr) {
      int err = rxerr;
      rxerr = 0;
      lock.release();
      return err;
    }
  }

  // We received FIN. It is EOF now.
  if (rxbuf.size == 0 && fin) {
    lock.release();
    return 0;
  }

  size_t n = min((unsigned) len, rxbuf.size);
  rxbuf.consume(buf, n);

  rxwindow = rxbuf.cap - rxbuf.size;
  lock.release();
  return n;
}

ssize_t tcp_socket_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  (void) offset;
  if (state != tcp::state::ESTABLISHED)
    return -ENOTCONN;
  if (len == 0 || len >= 65515)
    return -EINVAL;
  
  bool nonblock = flags & O_NONBLOCK;
  wait_entry entry;
  lock.acquire();
  // Wait till we have enough space.
  while (txbuf.cap == txbuf.size && state == tcp::state::ESTABLISHED) {
    if (nonblock) {
      lock.release();
      return -EAGAIN;
    }

    if (state != tcp::state::ESTABLISHED) {
      lock.release();
      return -ECONNRESET;
    }
    
    writewait.prepare(entry);
    lock.release();
    if (suspend() != 0)
      return -EINTR;
    lock.acquire();
    writewait.finish(entry);
  }

  // Put the message into buffer.
  auto l = min((unsigned) len, txbuf.cap - txbuf.size);
  txbuf.write(buf, l);
  lock.release();

  // Try send out a packet.
  for (;;) {
    unsigned unacked = seq - unack;
    unsigned window = txwindow - unacked;
    unsigned unsent = txbuf.size - unacked;
    unsigned tosend = min(mss, min(window, unsent));
    if (tosend <= 0)
      break;

    unsigned char flags = 0;
    // We've sent all of our data, so we request a push.
    if (unsent == tosend)
      flags |= tcp::PSH;
    send(txbuf, unacked, flags);
    seq += tosend;
  }
  return l;
}

int tcp_socket_inode::connect(ip::address addr, tcp::port port) {
  // Haven't bound yet; perform an implicit bind.
  if (state == tcp::state::CLOSED) {
    if (auto ret = bind(ip::src, 0); ret < 0)
      return ret;
  }

  // Already connected. Return.
  if (state != tcp::state::BOUND)
    return -EISCONN;

  // Start threeway handshaking. Initialize internal data first.
  dst = addr;
  dstport = port;
  seq = rand();
  ack = 0;
  rxwindow = 65535;

  // Receive the SYN + ACK package. Do a timed wait.
  int timeouts[] = { 1, 3, 7, 15, 30, 60 };
  auto tcb = active();
  wait_entry entry;
  bool received = false;
  char p[total];
  // Always leave some space to let IP/Ethernet directly write before p, without copying.
  auto q = p + total - sizeof(tcp::header);
  for (auto time : timeouts) {
    // Send the SYN package.
    tcp::fill_header(q, src, dst, srcport, dstport, htonl(seq), 0, tcp::SYN, htons(rxwindow), 0);
    ip::write(q, sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
    state = tcp::state::SYN_SENT;

    lock.acquire();
    for (;;) {
      readwait.prepare(entry);
      lock.release();
      
      // Woken up either on receive, or on timeout.
      int ret = tcb->sleep(time * 1_s);
      
      lock.acquire();
      readwait.finish(entry);
      if (/*awoken from sleeping, rather than data*/ ret == 0)
        rxerr = -ETIMEDOUT;
      if (rxerr != 0 || !recv.empty())
        break;
    }
    // An error happened, retry.
    if (rxerr != 0) {
      rxerr = 0;
      lock.release();
      continue;
    }

    // Look at the received message
    const auto *header = (const tcp::header *) recv.c_str();
    // Actively refused.
    if (header->flags & tcp::RST) {
      lock.release();
      return -ECONNREFUSED;
    }

    // This is not the one we're waiting for.
    if (!(header->flags & tcp::ACK) || !(header->flags & tcp::SYN) || ntohl(header->ack) != seq + 1) {
      lock.release();
      continue;
    }

    // This is alright.
    ack = ntohl(header->seq) + 1;
    txwindow = ntohs(header->window);
    received = true;

    lock.release();
    break;
  }

  if (!received)
    // Failed.
    return -ETIMEDOUT;
  
  // Send the final ACK package.
  tcp::fill_header(q, src, dst, srcport, dstport, htonl(++seq), htonl(ack), tcp::ACK, htons(rxwindow), 0);
  ip::write(q, sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
  hexdump(q, sizeof(tcp::header));
  unack = seq;
  state = tcp::state::ESTABLISHED;
  return 0;
}

void tcp_socket_inode::onclose(int) {
  switch (state) {
  case tcp::SYN_SENT:
  case tcp::BOUND:
    state = tcp::CLOSED;
    break;
  default:
    printk("unknown state %d\n", state);
  }
}

}
