#ifndef TCP_H
#define TCP_H

#include "net.h"

namespace os::tcp {

enum class state {
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

struct header {
  port srcport, dstport;
  unsigned src, ack;

  unsigned char __resv : 4;
  unsigned char offset : 4; // Data offset, which is equal to header length. Unit: 4 bytes
  unsigned char flags;
  unsigned short window;

  unsigned short checksum;
  unsigned short urgent_ptr;

  bool is_syn() const { return flags & 0x02; }
  bool is_ack() const { return flags & 0x10; }
  bool is_fin() const { return flags & 0x01; }
  bool is_rst() const { return flags & 0x04; }
};

}

namespace os {

class tcp_socket_inode : public inode_impl<tcp_socket_inode> {
  tcp::state state;
  
  ip::address dst, src;
  tcp::port dstport, srcport;

  unsigned unack, rxnext, rxwindow;
  unsigned txnext, txwindow;

  static tcp::port allocate();
public:
  tcp_socket_inode();

  int bind(ip::address addr, tcp::port port);
  int connect(ip::address addr, tcp::port port);
};

}

#endif
