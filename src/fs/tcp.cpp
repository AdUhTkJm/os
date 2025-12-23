#include "tcp.h"

namespace os {

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
  inode_impl(&sockfs, 0, 0, 0666, Socket), state(tcp::state::CLOSED), unack(0), rxnext(0), rxwindow(500), txnext(0), txwindow(500) {

}

int tcp_socket_inode::bind(ip::address addr, tcp::port port) {
  if (port == 0) {
    if (port = htons(allocate()); !port)
      return -EADDRINUSE;
  }
  if (demux->tcps.count(port))
    return -EADDRINUSE;

  src = addr;
  srcport = port;
  state = tcp::state::BOUND;
  return 0;
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

  // Start threeway handshaking.
  dst = addr;
  dstport = port;
  // TODO: Send the SYN package.
  return 0;
}

}
