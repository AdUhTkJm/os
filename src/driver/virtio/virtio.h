#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stddef.h>
#include "../../mem/ptable.h"
#include "../../fs/devfs.h"
#include "../../fs/net.h"

/*
For VirtIO specification, see
https://docs.oasis-open.org/virtio/virtio/v1.3/csd01/virtio-v1.3-csd01.pdf
*/

namespace os { struct pcb_t; }

namespace os::virtio {

enum device_status : int32_t {
  ACKNOWLEDGE = 1,
  DRIVER = 2,
  DRIVER_OK = 4,
  FEATURES_OK = 8,
  DEVICE_NEEDS_RESET = 64,
  FAILED = 128,
};

enum mmio_offsets {
  DEVICE_FEATURE = 0x10,
  DRIVER_FEATURE = 0x20,
  DEVICE_FEATURESEL = 0x14,
  DRIVER_FEATURESEL = 0x24,
  QUEUE_SEL = 0x30,
  QUEUE_SIZE_MAX = 0x34,
  QUEUE_SIZE = 0x38,
  QUEUE_READY = 0x44,
  QUEUE_NOTIFY = 0x50,
  INTERRUPT_STATUS = 0x60,
  INTERRUPT_ACK = 0x64,
  STATUS = 0x70,
  QUEUE_DESC_LOW = 0x80,
  QUEUE_DESC_HIGH = 0x84,
  QUEUE_DRIVER_LOW = 0x90,
  QUEUE_DRIVER_HIGH = 0x94,
  QUEUE_DEVICE_LOW = 0xa0,
  QUEUE_DEVICE_HIGH = 0xa4,
  QUEUE_RESET = 0xc0,
  CONFIG_GENERATION = 0xfc,
  CONFIG_BASE = 0x100,
};

enum block_device_legacy_offsets {
  DRIVER_PAGE_SIZE = 0x28,
  QUEUE_ALIGN = 0x3c,
  QUEUE_PFN = 0x40,
};

// SIZE_MAX is defined by <stdint.h>, so note the `SIZEMAX` here.
// See Section 5.2.3 for these feature bits of block devices.
enum block_features {
  SIZEMAX = 1 << 1,
  SEG_MAX = 1 << 2,
  GEOMETRY = 1 << 4,
  RDONLY = 1 << 5,
  BLK_SIZE = 1 << 6,
  FLUSH = 1 << 9,
  TOPOLOGY = 1 << 10,
  CONFIG_WCE = 1 << 11,
  MULTIQUEUE = 1 << 12,
  DISCARD = 1 << 13,
  WRITE_ZEROS = 1 << 14,
  LIFETIME = 1 << 15,
  SECURE_ERASE = 1 << 16,
  ZONED = 1 << 17,
};


enum features : unsigned long {
  VERSION_1 = 1ul << 32,
};

enum legacy_features {
  ANY_LAYOUT = 1 << 27
};

// The structure from FDT tree.
struct device {
  const char *compatible;
  pa_t base;
  size_t len;
  int interrupt;
};

// Description of virtqueue.
// See section 2.7.
namespace vq {

constexpr int size = 32;
constexpr int align = PAGE_SIZE;

// This describes one buffer.
// This will be sent to device for them to process. `next` can be used to chain
// multiple descriptors together as a single request.
struct desc {
  pa_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

enum descflags {
  HAS_NEXT = 1,
  WRITEONLY = 2,
  INDIRECT = 4
};

// The driver uses the available ring to offer buffers to the device.
// It is driver-write-only.
// This is a ring buffer of indices of descriptors, and `idx` points to where the
// next request would be placed.
struct avail_ring {
  uint16_t flags;
  uint16_t idx;
  uint16_t ring[size];
  uint16_t used_event;
};

struct used_ring {
  uint16_t flags;
  uint16_t idx;
  struct element {
    uint32_t id;
    uint32_t len;
  } ring[size];
  uint16_t avail_event;
};

// The virtual queue.
struct queue {
  vq::desc (*desc)[vq::size];
  vq::avail_ring *avail;
  vq::used_ring *used;
};

struct queue_legacy {
  vq::desc desc[vq::size];
  vq::avail_ring avail;
  char padding[PAGE_SIZE - sizeof(desc) - sizeof(avail)];
  vq::used_ring used;
};
static_assert(offsetof(queue_legacy, used) % PAGE_SIZE == 0);

}

class block_device : public os::block_device {
  pa_t base;
  vq::queue_legacy *q;
  unsigned last = 0;
  int descid;
  bool legacy;
  spinlock readlock, writelock;
  wait_queue readwait, writewait;
  unsigned long cap;
  unsigned segment_size_max;

  // Track read requests.
  // Maps `head` to the request's wait entry.
  wait_entry *readreq[vq::size], *writereq[vq::size];

  // Track free descriptors.
  using descriptor = uint16_t;
  descriptor free[vq::size];
  unsigned short head;
  unsigned short count;
  descriptor next[vq::size];

  bool alloc_chain(int n, descriptor *desc);
  void free_chain(descriptor desc);

  struct request {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t  *data;
    uint8_t  status;
  };
  
  struct request_legacy {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
  };
  static_assert(sizeof(request_legacy) == 16);

  int read_legacy(uint64_t lba, void *buffer, int len);
  int write_legacy(uint64_t lba, const void *buffer, int len);
  friend void block_device_handler(int irq);
public:
  block_device(const device &, bool legacy);
  block_device(const block_device &) = delete;
  block_device &operator=(const block_device &) = delete;

  // For operation specifications, see 5.2.6.
  int read(uint64_t lba, void *buffer, int len) override;
  int write(uint64_t lba, const void *buffer, int len) override;
};

class net_device : public os::net_device {
  pa_t base;
  vq::queue_legacy *rx, *tx; // Receive/transmit queues.
  unsigned rxnext = 0, rxlast = 0, txlast = 0;
  bool legacy;
  spinlock lock;
  wait_queue txwait;
  unsigned txid = 0;

  unsigned next_tx_descriptor();
  static constexpr size_t PACKET_BUF_SIZE = 2048;
  pa_t rxbuf[vq::size];
  pa_t txbuf[vq::size];

  struct header {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
  };

  // See Section 5.1.3.
  enum features {
    MAC = 1 << 5,
    FSTATUS = 1 << 16,
  };

  enum headers {
    NEEDS_CSUM = 1,
    DATA_VALID = 2,
    RSC_INFO = 4
  };

  enum gso {
    NONE = 0,
    TCPV4 = 1,
    UDP = 3,
    TCPV6 = 4,
    UDP_L4 = 5,
    ECN = 0x80
  };

  static constexpr int RXID = 0, TXID = 1;
  friend void net_device_handler(int irq);
public:
  net_device(const device &, bool legacy);
  net_device(const net_device &) = delete;
  net_device &operator=(const net_device &) = delete;

  int read();
  int write(const void *buf, int len, bool block) override;
  bool write_full() override;
  void wake_write() override;
};

net_device *netdev(int i = 0);

void probe();

}

#endif
