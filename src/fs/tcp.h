#ifndef TCP_H
#define TCP_H

#include "net.h"
#include "../utils/stl/btree.h"

// See Computers Networking, A Systems Approach:
//   https://book.systemsapproach.org/e2e/tcp.html

namespace os::tcp {

enum state {
  CLOSED,
  BOUND,
  LISTEN,
  SYN_SENT,
  SYN_RECEIVED,
  ESTABLISHED,
  FIN_WAIT_1,
  FIN_WAIT_2,
  CLOSE_WAIT,
  CLOSING,
  LAST_ACK,
  TIME_WAIT
};

using port = uint16_t;

enum flags {
  FIN = 0x01,
  SYN = 0x02,
  RST = 0x04,
  PSH = 0x08,
  ACK = 0x10,
};

struct header {
  port srcport, dstport;
  unsigned seq, ack;

  unsigned char __resv : 4;
  unsigned char offset : 4; // Data offset, which is equal to header length. Unit: 4 bytes
  unsigned char flags;
  unsigned short window;

  unsigned short checksum;
  unsigned short urgent_ptr;
};

void fill_header(char *p,
  ip::address src, ip::address dst, port srcport, port dstport,
  unsigned seq, unsigned ack, unsigned char flags,
  unsigned short window, unsigned short  payload_len
);

void read(const char *header, size_t len, int error);

}

namespace os {

class tcp_socket_inode : public inode_impl<tcp_socket_inode> {
  tcp::state state;

  // Remember to convert them on send:
  //
  // `ack` is the offset of the next byte we expect to receive, i.e. the `ack` field in header.
  __little unsigned ack;
  // `seq` is the offset of the next byte we'll transmit, i.e. the `seq` field in header.
  // `acked` is the last byte we've acked. Note this is different from `ack`.
  __little unsigned acked, seq, txwindow;
  unsigned short mss = 1460;
  unsigned short rxwindow() { return rxbuf.cap - rxbuf.size; }

  option options;
  bool fin = false;
  
  using packet = string;
  spinlock lock;
  int rxerr = 0;
  // Raw packet, for handshaking.
  packet recv;
  wait_queue readwait, writewait;
  // A scratch buffer to hold the connection.
  char *scratch;

  // The out-of-order packet queue. It has to be sorted, so we use a B-tree here.
  os::btree<unsigned, packet, 8> ooo;

  // The ring buffer.
  struct buffer {
    char *data;
    unsigned head = 0, tail = 0, size = 0, cap;

    void consume(void *buf, unsigned len);
    void peek(void *buf, unsigned offset, unsigned len) const;
    void write(const void *buf, unsigned len);
    void discard(unsigned len);
  } rxbuf, txbuf;

  static tcp::port allocate();
  // Sends a single packet.
  ssize_t send(const buffer &buf, unsigned offset, unsigned len, unsigned char flags);
  
  void receive(packet &&data);
  void receive(int error);

  void consume(const char *p, size_t len);

  // The total amount of headers. (TODO: what if we're not on IP/Ethernet?)
  static constexpr auto total = sizeof(tcp::header) + sizeof(ip::header) + sizeof(eth::header);
  friend class demux;
  friend void tcp::read(const char *p, size_t len, int error);
public:
  ip::address dst, src;
  tcp::port dstport, srcport;

  FILE_INODE_DEFAULT_IMPL;
  tcp_socket_inode();
  ~tcp_socket_inode();

  ssize_t read(size_t offset, void *buf, size_t len, int flags) override;
  ssize_t write(size_t offset, const void *buf, size_t len, int flags) override;
  void set_meta(const meta &) override {}
  meta get_meta() override { return meta(0, 0, 0); }

  void prepare_read_wait(wait_entry &entry) override { readwait.prepare(entry); }
  void wake_read() override { readwait.wake_all(); }
  void onclose(int) override;

  int bind(ip::address addr, tcp::port port);
  int connect(ip::address addr, tcp::port port);

  tcp::state get_state() const { return state; }
};

}

#endif
