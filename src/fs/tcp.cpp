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
  if (error != 0) {
    node->receive(error);
    return;
  }

  // We don't strip header here, unlike UDP; we need its info inside the inode.
  tcp_socket_inode::packet g(p, len);
  node->receive(os::move(g));
}

// Avoid `mod` on every iteration step.
void tcp_socket_inode::buffer::write(const void *buf, unsigned len) {
  assert(size <= cap - len);
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
  peek(buf, 0, len);
  discard(len);
}

void tcp_socket_inode::buffer::peek(void *buf, unsigned offset, unsigned len) const {
  auto dst = (char *) buf;
  offset = (offset + head) % cap;
  unsigned chunk = min((unsigned) len, cap - offset);
  memcpy(dst, data + offset, chunk);
  if (len > chunk)
    memcpy(dst + chunk, data, len - chunk);
}

void tcp_socket_inode::buffer::discard(unsigned len) {
  assert(size >= len);
  head += len;
  if (head >= cap)
    head -= cap;
  size -= len;
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
  scratch = new char[mss + total];
}

tcp_socket_inode::~tcp_socket_inode() {
  delete[] rxbuf.data;
  delete[] txbuf.data;
  delete[] scratch;
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
  lock.acquire();
  
  if (state != tcp::ESTABLISHED)
    // We're not expecting messages, but headers.
    // consume() would strip them, so we'd rather store the original packet.
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

ssize_t tcp_socket_inode::send(const buffer &buf, unsigned offset, unsigned len, unsigned char flags) {
  // Create a large enough buffer.
  char *p = scratch + total;
  buf.peek(p, offset, len);
  p -= sizeof(tcp::header);
  
  tcp::fill_header(p, src, dst, srcport, dstport, htonl(seq), htonl(ack), flags, htons(rxwindow()), len);
  return ip::write(p, len + sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
}

void tcp_socket_inode::consume(const char *p, size_t len) {
  // Verify packet.
  const auto *header = (const tcp::header *) p;
  // Incoming sequence number, not "is-eq".
  unsigned iseq = ntohl(header->seq);
  unsigned iack = ntohl(header->ack);
  // Calculate payload length.
  len -= header->offset * 4;
  p += header->offset * 4;
  if (len > rxwindow()) {
    // The packet should be no longer than advertised window; drop this packet.
    printk("tcp: warning: packet malformed, dropped\n");
    return;
  }
  
  bool pureack = false;

  lock.acquire();
  if (header->flags & tcp::ACK) {
    if (iack > acked) {
      txbuf.discard(iack - acked);
      acked = iack;
      writewait.wake_all();
      txwindow = ntohs(header->window);
    }
    // Otherwise, this is a duplicate ACK. Safe to do nothing - but what else to do?
  }

  // We received in-order data.
  if (iseq == ack) {
    rxbuf.write(p, len);
    ack += len;
    pureack = true;
    // Try to consume out-of-order buffer.
    for (;;) {
      auto *str = ooo.find(ack); 
      if (!str)
        break;

      rxbuf.write(str->c_str(), str->size());
      ooo.erase(ack);
      ack += str->size();
    }
  }

  // We received out-of-order data.
  else if (iseq > ack)
    ooo.insert(iseq, string(p, len));

  // We can ignore duplicate data when iseq < ack.

  // Deal with FIN. This changes `ack` so must be handled after receiving data.
  if (header->flags & tcp::FIN) {
    ack++;
    state = tcp::state::CLOSE_WAIT;
    pureack = true;
  }
  lock.release();

  // Send an ACK.
  if (pureack) {
    char ackbuf[total];
    char *q = ackbuf + total - sizeof(tcp::header);
    tcp::fill_header(q, src, dst, srcport, dstport, htonl(seq), htonl(ack), tcp::ACK, htons(rxwindow()), 0);
    ip::write(q, sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
  }
}

ssize_t tcp_socket_inode::read(size_t offset, void *buf, size_t len, int flags) {
  (void) offset;
  bool nonblock = flags & O_NONBLOCK;
  if (len == 0)
    return -EINVAL;

  wait_entry entry;
  lock.acquire();
  while (rxbuf.size == 0 && state == tcp::state::ESTABLISHED) {
    if (nonblock) {
      lock.release();
      return -EAGAIN;
    }

    hangon(readwait, lock, entry);

    if (rxerr) {
      int err = rxerr;
      rxerr = 0;
      lock.release();
      return err;
    }
  }

  // We received FIN and no more data will come. It is EOF now.
  if (rxbuf.size == 0 && state != tcp::state::ESTABLISHED) {
    lock.release();
    return 0;
  }

  size_t n = min((unsigned) len, rxbuf.size);
  rxbuf.consume(buf, n);

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
    unsigned inflight = seq - acked;
    unsigned short window = txwindow - inflight;
    unsigned short unsent = txbuf.size - inflight;
    unsigned short tosend = min(mss, min(window, unsent));
    if (tosend <= 0)
      break;

    unsigned char flags = tcp::ACK;
    // We've sent all of our data, so we request a push.
    if (unsent == tosend)
      flags |= tcp::PSH;
    
    synchronized _(lock);
    if (auto ret = send(txbuf, inflight, tosend, flags); ret < 0)
      return ret;
    seq += tosend;
  }
  return l;
}

short tcp_socket_inode::poll(unsigned short events) {
  short result = 0;
  if ((events & POLLIN) && rxbuf.size != 0)
    result |= POLLIN;
  if ((events & POLLOUT) && txbuf.size != txbuf.cap)
    result |= POLLOUT;
  return result;
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

  // Receive the SYN + ACK package. Do a timed wait.
  int timeouts[] = { 1, 3, 7, 15, 30, 60 };
  auto tcb = active();
  wait_entry entry;
  bool received = false;
  char p[total + 4];
  // Always leave some space to let IP/Ethernet directly write before p, without copying.
  // Form the SYN package. We don't use fill_header() helper, because we aren't using a 20-byte header here.
  auto q = p + total - 24;
  auto header = (tcp::header *) q;
  header->srcport = srcport;
  header->dstport = dstport;
  header->seq = htonl(seq);
  header->__resv = 0;
  header->offset = 6; // We're including options.
  header->ack = 0;
  header->window = htons(rxwindow());
  header->flags = tcp::SYN;
  header->urgent_ptr = 0;
  header->checksum = 0;
  // MSS (option 2): 0x05b4 (1460 bytes)
  q[20] = 0x02; q[21] = 0x04; q[22] = 0x05; q[23] = 0xb4;
  // Don't forget the pseudo-header.
  unsigned sum = 0;
  ip::pseudo_header h { .src = src, .dst = dst, .zero = 0, .prot = TCP, .len = htons(24) };
  sum = ip::checksum_add(sum, &h, 12);
  sum = ip::checksum_add(sum, q, 24);
  header->checksum = ip::checksum_fold(sum);

  for (auto time : timeouts) {
    // (Re)send the SYN packet.
    ip::write(q, 24, src, dst, protocol::TCP, 0, options);
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

    // Look at the received message.
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

    // This is alright. Let's fill in our data.
    ack = ntohl(header->seq) + 1;
    txwindow = ntohs(header->window);
    received = true;

    // Look at options, if present.
    auto end = recv.c_str() + recv.size();
    for (auto p = (const char *) (header + 1); p < end;) {
      unsigned char option = *p;
      unsigned char len = *(p + 1);
      p += 2;
      switch (option) {
      case 2: // MSS size
        if (len != 4)
          continue;
        
        mss = min(mss, htons(*(unsigned short *) p));
        p += 2;
        break;
      default:
        printk("tcp: unknown option: %d\n", option);
      }
    }
    lock.release();
    break;
  }

  if (!received)
    // Failed.
    return -ETIMEDOUT;

  // The SYN packet consumes a sequence number.
  ++seq;
  
  // Send the final ACK package. It doesn't consume a sequence number.
  tcp::fill_header(q, src, dst, srcport, dstport, htonl(seq), htonl(ack), tcp::ACK, htons(rxwindow()), 0);
  ip::write(q, sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
  acked = seq;
  state = tcp::state::ESTABLISHED;
  return 0;
}

void tcp_socket_inode::onclose(int) {
  switch (state) {
  case tcp::SYN_SENT:
  case tcp::BOUND:
  case tcp::CLOSED:
    state = tcp::CLOSED;
    break;
  case tcp::CLOSE_WAIT: {
    char p[total];
    char *q = p + total - sizeof(tcp::header);
    tcp::fill_header(q, src, dst, srcport, dstport, htonl(seq), htonl(ack), tcp::FIN, htons(rxwindow()), 0);
    ip::write(q, sizeof(tcp::header), src, dst, protocol::TCP, 0, options);
    seq++;
    state = tcp::LAST_ACK;
    break;
  }
  default:
    printk("unknown state %d\n", state);
  }
}

}
