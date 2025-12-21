#include "virtio.h"
#include "../plic/plic.h"
#include "../../fs/devfs.h"
#include "../../fs/net.h"
#include "../../fdt/fdt.h"
#include "../../mem/kalloc.h"
#include "../../proc/schedule.h"

namespace {

uint32_t read(uint32_t *p) {
  return to_big_endian(*p);
}

int block_device_cnt = 0;
int net_device_cnt = 0;

}

namespace os::virtio {

static_storage<os::hashmap<int, block_device*>> blk_intr;
static_storage<os::hashmap<int, net_device*>> net_intr;
static_storage<os::hashmap<int, net_device*>> net_devs;

[[gnu::no_instrument_function]] void block_device_handler(int irq) {
  if (!blk_intr->count(irq))
    return;

  block_device *dev = blk_intr->at(irq);
  // Note that status bit 0 (value 1) means a used ring change;
  // bit 1 (value 2) means a configuration change.
  //
  // Also note that used ring change won't be handled here.
  // That's the job of the function after recovery from suspend().
  int status = mmrd<uint32_t>(dev->base + INTERRUPT_STATUS);
  if (!(status & 1))
    return;
  mmwr(dev->base + INTERRUPT_ACK, status);
  os::mmwr(PLIC_BASE + PLIC_CLAIM_S_OFFSET, irq);
  dev->wait.wake_all();
}

[[gnu::no_instrument_function]] void net_device_handler(int irq) {
  if (!net_intr->count(irq))
    return;

  net_device *dev = net_intr->at(irq);
  int status = mmrd<uint32_t>(dev->base + INTERRUPT_STATUS);
  if (!(status & 1))
    return;
  mmwr(dev->base + INTERRUPT_ACK, status);
  os::mmwr(PLIC_BASE + PLIC_CLAIM_S_OFFSET, irq);
  
  if (dev->rxlast != dev->rx->used.idx) {
    dev->rxlast = dev->rx->used.idx;
    dev->read();
  }

  // A write has occurred.
  if (dev->txlast != dev->tx->used.idx) {
    dev->txlast = dev->tx->used.idx;
    dev->txwait.wake_all();
  }

  scheduler.maybe_preempt();
}

// Configures the underlying device.
// See section 3.1.1.
block_device::block_device(const device &device, bool legacy): descid(0), legacy(legacy) {
  base = device.base;

  // Reset the device.
  int status = 0;
  mmwr(base + STATUS, status);

  // Set the acknowledge bit.
  status |= device_status::ACKNOWLEDGE;
  mmwr(base + STATUS, status);

  // Set the driver status bit.
  status |= device_status::DRIVER;
  mmwr(base + STATUS, status);

  // Negotiate features.
  
  // Tell the device of our supported features.

  mmwr(base + DEVICE_FEATURESEL, 0);
  auto features_low = mmrd<uint32_t>(base + DEVICE_FEATURE);
  bool anylayout = features_low & legacy_features::ANY_LAYOUT;

  uint64_t supported = anylayout ? legacy_features::ANY_LAYOUT : 0;
  mmwr(base + DRIVER_FEATURESEL, 0);
  mmwr<uint32_t>(base + DRIVER_FEATURE, supported);

  // In legacy mode, there is no such selection.
  if (!legacy) {
    mmwr(base + DEVICE_FEATURESEL, 1);
    auto features_high = mmrd<uint32_t>(base + DEVICE_FEATURE);
    // Legacy should be indicated by `VERSION_1` not offered.
    assert(bool(features_high & (((uint64_t) features::VERSION_1) >> 32)) ^ legacy);

    mmwr(base + DRIVER_FEATURESEL, 1);
    mmwr<uint32_t>(base + DRIVER_FEATURE, supported >> 32);
  }

  // Mark out intent as "complete".
  // This is only done in v1.0 standard.
  if (!legacy) {
    status |= device_status::FEATURES_OK;
    mmwr(base + STATUS, status);
    // Reread to see whether the device accepts it. (It must for now; we didn't use any features.)
    int accepted = mmrd<int32_t>(base + STATUS);
    if (!(accepted & device_status::FEATURES_OK))
      panic("virtio device failed");
  }

  // Do device-specific set up. See section 4.2.3.2.
  // Select virtqueue 0, and see max queue size.
  mmwr(base + QUEUE_SEL, 0);
  if (!legacy && mmrd<int32_t>(base + QUEUE_READY) != 0)
    panic("block device: queue occupied");

  if (legacy && mmrd<int32_t>(base + QUEUE_PFN) != 0)
    panic("block device: queue occupied");

  auto maxsz = mmrd<int32_t>(base + QUEUE_SIZE_MAX);
  if (vq::size > maxsz)
    panic("virtqueue size too large");

  // Set the queue length.
  mmwr(base + QUEUE_SIZE, vq::size);

  // We assume a split virtqueue.
  // Allocate the three areas. See section 2.7.
  if (!legacy) {
    // TODO: This has to be physically contiguous.
    pa_t desc = to_pa(vmalloc<16>(16 * vq::size));
    pa_t avail = to_pa(vmalloc<2>(6 + 2 * vq::size));
    pa_t used = to_pa(vmalloc<4>(6 + 8 * vq::size));

    mmwr<uint32_t>(base + QUEUE_DESC_LOW, desc & 0xffff'ffff);
    mmwr<uint32_t>(base + QUEUE_DESC_HIGH, desc >> 32);
    mmwr<uint32_t>(base + QUEUE_DRIVER_LOW, avail & 0xffff'ffff);
    mmwr<uint32_t>(base + QUEUE_DRIVER_HIGH, avail >> 32);
    mmwr<uint32_t>(base + QUEUE_DEVICE_LOW, used & 0xffff'ffff);
    mmwr<uint32_t>(base + QUEUE_DEVICE_HIGH, used >> 32);
    queue = new (os::permanent) vq::queue {
      (vq::desc(*)[vq::size]) as_va(desc),
      (vq::avail_ring*) as_va(avail),
      (vq::used_ring*) as_va(used)
    };

    // Now the queue is ready.
    mmwr(base + QUEUE_READY, 1);
  } else {
    // See section 2.7.2 for the legacy layout.
    mmwr(base + QUEUE_ALIGN, vq::align);
    mmwr(base + DRIVER_PAGE_SIZE, PAGE_SIZE);

    // Allocate space for queue.
    constexpr auto size = roundup<vq::align>(sizeof(vq::desc) * vq::size + 2 * (3 + vq::size))
      + roundup<vq::align>(6 + sizeof(vq::used_ring::element) * vq::size);
    static_assert(roundup<PAGE_SIZE>(sizeof(vq::queue_legacy)) == size);
    pa_t mem = pmalloc(size / PAGE_SIZE);

    // Set up the registers and the queue.
    mmwr<uint32_t>(base + QUEUE_PFN, mem / PAGE_SIZE);
    queue = (vq::queue_legacy *) as_va(mem);
    memset(queue, 0, size);

    // In legacy we shouldn't set up QueueReady.
  }

  // Finish the setup.
  status |= device_status::DRIVER_OK;
  mmwr(base + STATUS, status);
}

vq::desc &block_device::next_descriptor() {
  // The non-legacy part is NYI.
  // TODO: Test whether queue is full?
  auto queue = (vq::queue_legacy*) this->queue;
  auto &desc = queue->desc[descid];
  descid = (descid + 1) % vq::size;
  return desc;
}

uint16_t block_device::indexof(const vq::desc &desc) {
  auto queue = (vq::queue_legacy*) this->queue;
  return &desc - queue->desc;
}

unsigned net_device::next_tx_descriptor() {
  auto desc = txid;
  txid = (txid + 1) % vq::size;
  return desc;
}

// The lba is the logical block address.
int block_device::read_legacy(uint64_t lba, void *buffer) {
  // We have to follow the layout specified by 5.2.6.4 for legacy drivers.
  // We need to ensure that they lie in the same page, so we can't just put them on `ksp`.
  // But it doesn't matter for `status` - one byte always works.
  pa_t req = pframe();
  pa_t buf = pframe();
  *(request_legacy*) as_va(req) = {
    .type = 0, /* Read */
    .reserved = 0,
    .sector = lba,
  };
  
  unsigned char status = 0xff;

  vq::desc &d1 = next_descriptor();
  vq::desc &d2 = next_descriptor();
  vq::desc &d3 = next_descriptor();
  d1.addr = req;
  d1.len = sizeof(request_legacy);
  d1.flags = vq::descflags::HAS_NEXT;
  d1.next = indexof(d2);

  d2.addr = buf;
  d2.len = 512;
  d2.flags = vq::descflags::HAS_NEXT | vq::descflags::WRITEONLY;
  d2.next = indexof(d3);

  d3.addr = to_pa(&status);
  d3.len = 1;
  d3.flags = vq::descflags::WRITEONLY;
  d3.next = 0;

  // Put the head of chain into available ring for the device to read.
  auto queue = (vq::queue_legacy*) this->queue;
  uint16_t head = indexof(d1);
  queue->avail.ring[queue->avail.idx % vq::size] = head;
  queue->avail.idx++;

  // Tell device that a new request has come.
  FENCE;
  mmwr(base + QUEUE_NOTIFY, /*queue_index=*/0);

  wait_entry entry;
  for (int i = 0;;) {
    lock.acquire();
    wait.prepare(entry);
    lock.release();
    if (suspend() != 0)
      return -EINTR;

    lock.acquire();
    wait.finish(entry);
    RFENCE;
    for (uint16_t last = queue->used.idx; i != last; i++) {
      vq::used_ring::element used = queue->used.ring[i % vq::size];
      
      if (used.id == head) {
        lock.release();
        if (status == 0)
          memcpy(buffer, (void *) as_va(buf), 512);

        pfree(req);
        pfree(buf);
        return status;
      }
    }
  }
}

int block_device::write_legacy(uint64_t lba, const void *buffer) {
  // We have to follow the layout specified by 5.2.6.4 for legacy drivers.
  pa_t req = pframe();
  pa_t buf = pframe();
  *(request_legacy*) as_va(req) = {
    .type = 1, /* Write */
    .reserved = 0,
    .sector = lba,
  };
  
  int status = 0xff;
  memcpy((void *) as_va(buf), buffer, 512);

  vq::desc &d1 = next_descriptor();
  vq::desc &d2 = next_descriptor();
  vq::desc &d3 = next_descriptor();
  d1.addr = req;
  d1.len = sizeof(request_legacy);
  d1.flags = vq::descflags::HAS_NEXT;
  d1.next = indexof(d2);

  d2.addr = buf;
  d2.len = 512;
  d2.flags = vq::descflags::HAS_NEXT;
  d2.next = indexof(d3);

  d3.addr = to_pa(&status);
  d3.len = 1;
  d3.flags = vq::descflags::WRITEONLY;
  d3.next = 0;

  // Put the head of chain into available ring for the device to read.
  // TODO: check queue full
  auto queue = (vq::queue_legacy*) this->queue;
  queue->avail.ring[queue->avail.idx % vq::size] = indexof(d1);
  queue->avail.idx++;

  // Tell device that a new request has come.
  WFENCE;
  mmwr(base + QUEUE_NOTIFY, /*queue_index=*/0);
  pfree(buf);
  pfree(req);
  return status;
}

int block_device::read(uint64_t lba, void *buffer) {
  if (legacy)
    return read_legacy(lba, buffer);
  assert(false && "NYI for non-legacy devices");
}

int block_device::write(size_t lba, const void *buffer) {
  if (legacy)
    return write_legacy(lba, buffer);
  assert(false && "NYI for non-legacy devices");
}

net_device::net_device(const device &dev, bool legacy): legacy(legacy) {
  base = dev.base;
  
  // The initial parts are identical to that of block device.
  // Reset device
  // Reset the device.
  int status = 0;
  mmwr(base + STATUS, status);

  // Set the acknowledge bit.
  status |= device_status::ACKNOWLEDGE;
  mmwr(base + STATUS, status);

  // Set the driver status bit.
  status |= device_status::DRIVER;
  mmwr(base + STATUS, status);

  // Negotiate features.
  
  mmwr(base + DEVICE_FEATURESEL, 0);
  auto features_low = mmrd<uint32_t>(base + DEVICE_FEATURE);

  uint64_t supported = 0;
  if (features_low & legacy_features::ANY_LAYOUT)
    supported |= legacy_features::ANY_LAYOUT;
  if (features_low & features::MAC)
    supported |= features::MAC;
  if (features_low & features::FSTATUS)
    supported |= features::FSTATUS;

  mmwr(base + DRIVER_FEATURESEL, 0);
  mmwr<uint32_t>(base + DRIVER_FEATURE, supported);

  // In legacy mode, there is no such selection.
  if (!legacy) {
    mmwr(base + DEVICE_FEATURESEL, 1);
    auto features_high = mmrd<uint32_t>(base + DEVICE_FEATURE);
    // Legacy should be indicated by `VERSION_1` not offered.
    assert(bool(features_high & (((uint64_t) virtio::features::VERSION_1) >> 32)) ^ legacy);

    mmwr(base + DRIVER_FEATURESEL, 1);
    mmwr<uint32_t>(base + DRIVER_FEATURE, supported >> 32);
  }

  mmwr(base + DRIVER_PAGE_SIZE, PAGE_SIZE);

  // Calculate space for queue.
  constexpr auto size = roundup<vq::align>(sizeof(vq::desc) * vq::size + 2 * (3 + vq::size))
    + roundup<vq::align>(6 + sizeof(vq::used_ring::element) * vq::size);
  static_assert(roundup<PAGE_SIZE>(sizeof(vq::queue_legacy)) == size);

  // Now set up two queues.
  vq::queue_legacy **qs[2] = { &rx, &tx };
  for (int i = 0; i < 2; i++) {
    // Select queue i. Configurations will work for this queue.
    mmwr(base + QUEUE_SEL, i);

    if (legacy && mmrd<int32_t>(base + QUEUE_PFN) != 0)
      panic("net device: queue occupied");
    
    auto maxsz = mmrd<int32_t>(base + QUEUE_SIZE_MAX);
    if (vq::size > maxsz)
      panic("virtqueue size too large");
    mmwr(base + QUEUE_SIZE, vq::size);

    mmwr(base + QUEUE_ALIGN, vq::align);
    pa_t mem = pmalloc(size / PAGE_SIZE);

    // Set up receive queue.
    mmwr<uint32_t>(base + QUEUE_PFN, mem / PAGE_SIZE);
    *qs[i] = (vq::queue_legacy *) as_va(mem);
    memset(*qs[i], 0, size);
  }

  // Read configuration to know the MAC address of the device.
  // We must loop until the configuration gets stable.
  unsigned generation;
  unsigned mac1 = -1;
  unsigned short mac2 = -1;
  if (!(supported & features::MAC))
    panic("net device: do not know host mac");

  // By section 2.5.4, legacy interface does not have a "generation" field.
  if (legacy) {
    unsigned old_mac1;
    unsigned short old_mac2;
    do {
      old_mac1 = mac1;
      old_mac2 = mac2;
      mac1 = mmrd<unsigned>(base + CONFIG_BASE);
      mac2 = mmrd<unsigned short>(base + CONFIG_BASE + 4);
    } while (mac1 != old_mac1 || mac2 != old_mac2);
  } else do {
    generation = mmrd<unsigned>(base + CONFIG_GENERATION);

    // See section 5.1.4, device configuration layout.
    // The MAC address are the first 6 bytes of the configuration space.
    mac1 = mmrd<unsigned>(base + CONFIG_BASE);
    mac2 = mmrd<unsigned short>(base + CONFIG_BASE + 4);
  } while (generation != mmrd<unsigned>(base + CONFIG_GENERATION));

  // Remember that VirtIO (legacy) uses host endian, not net endian.
  // So we're ok here.
  memcpy(eth::src, &mac1, 4);
  memcpy(eth::src + 4, &mac2, 2);

  if (supported & features::FSTATUS) {
    // By section 2.5.4, we probably don't need to read this multiple times.
    unsigned short status = mmrd<unsigned short>(base + CONFIG_BASE + 6);
    bool active = status & 1;
    if (!active)
      panic("net device: link not active");
  }

  // Allocate space for transmit/receive queue.
  // Each buffer is 2048 byte long and occupies half a page.
  for (int i = 0; i < vq::size; i += 2) {
    pa_t page = pframe();
    rxbuf[i] = page;
    rxbuf[i + 1] = page + PAGE_SIZE / 2;

    page = pframe();
    txbuf[i] = page;
    txbuf[i + 1] = page + PAGE_SIZE / 2;
  }

  // Fill receive queue with buffers before starting.
  // The device needs empty buckets to pour incoming packets into.
  // Don't do it for the transmit queue; write() will fill it in.
  for (int i = 0; i < vq::size; i++) {
    rx->desc[i].addr = rxbuf[i];
    rx->desc[i].len = PACKET_BUF_SIZE;
    rx->desc[i].flags = vq::descflags::WRITEONLY;
    rx->desc[i].next = 0;

    rx->avail.ring[i] = i;
  }
  rx->avail.idx = vq::size;
  
  // Set up complete.
  status |= DRIVER_OK;
  mmwr(base + STATUS, status);
  
  // Send the available ring change to device.
  FENCE;
  mmwr(base + QUEUE_NOTIFY, RXID);
}

int net_device::read() {
  // We have some data.
  auto elem = rx->used.ring[rxnext++ % vq::size];
  RFENCE;

  // Read the packet, and skip the header.
  unsigned lpacket = elem.len - sizeof(header);
  if (lpacket > 0) {
    // Send this to the demultiplexer. It owns the pointer.
    auto buf = new char[lpacket];
    memcpy(buf, (char*) as_va(rxbuf[elem.id]) + sizeof(header), lpacket);
    demux->push(buf, lpacket);
  }

  // Now the descriptor is free. Return it to available ring.
  rx->avail.ring[rx->avail.idx % vq::size] = elem.id;
  rx->avail.idx++;
  
  WFENCE;
  mmwr(base + QUEUE_NOTIFY, RXID);
  return lpacket;
}

void net_device::wake_write() {
  txwait.wake_all();
}

int net_device::write(const void *buf, int len, bool block) {
  if (unsigned(len + sizeof(header)) >= PACKET_BUF_SIZE)
    return -E2BIG;

  wait_entry entry;
  lock.acquire();
  for (;;) {
    // The queue is full. Suspend.
    if (tx->avail.idx - txlast >= vq::size) {
      if (!block) {
        lock.release();
        return -ENOSPC;
      }

      txwait.prepare(entry);
      lock.release();
      if (suspend() != 0)
        return -EINTR;

      lock.acquire();
      txwait.finish(entry);
      continue;
    }
    
    auto index = next_tx_descriptor();
    auto &desc = tx->desc[index];

    // QEMU hopes the header and data are placed in two separate buffers.
    header *hbuf = (header *) as_va(txbuf[index]);
    *hbuf = {
      .flags = 0,
      .gso_type = gso::NONE,
      .hdr_len = sizeof(header),
      .gso_size = 0,
      .csum_start = 0, /* No checksum for now. */
      .csum_offset = 0,
    };
    memcpy(hbuf + 1, buf, len);

    // Transmit a request.
    desc.addr = txbuf[index];
    desc.len = sizeof(header) + len;
    desc.flags = 0;
    desc.next = 0;

    tx->avail.ring[tx->avail.idx % vq::size] = index;
    tx->avail.idx++;
    WFENCE;
    mmwr(base + QUEUE_NOTIFY, TXID);
    lock.release();
    return len;
  }
}

bool net_device::write_full() {
  return tx->avail.idx - txlast >= vq::size;
}

void probe() {
  char *p = (char *) fdt::pfdt + to_big_endian(fdt::pfdt->off_dt_struct);
  os::hashmap<string, device> devs;
  fdt::walk(p, [&](const char *cdev, const char *cprop, void *property, int len) {
    if (strncmp(cdev, "/soc/virtio_mmio@", 17) != 0)
      return WalkResult::Continue;

    // Now this is a virtio device.
    if (strcmp(cprop, "compatible") == 0)
      devs[cdev].compatible = (const char*) property;

    if (strcmp(cprop, "reg") == 0) {
      // TODO: read /soc #address-size.
      assert(len == 16);

      devs[cdev].base = (read((uint32_t*) property) * 1ull << 32)
        | read((uint32_t*) property + 1);
      devs[cdev].len = (read((uint32_t*) property + 2) * 1ull << 32)
        | read((uint32_t*) property + 3);
    }

    if (strcmp(cprop, "interrupts") == 0)
      devs[cdev].interrupt = to_big_endian(*(uint32_t*) property);

    return WalkResult::Continue;
  });

  auto dentry = active()->pcb->vfs->lookup("/dev");
  if (!dentry)
    panic("virtio: cannot find /dev");
  auto devnode = dyn_cast<devroot>((*dentry)->node);
  if (!devnode)
    panic("virtio: /dev not properly mounted");

  // We only consider mmio devices for now.
  // See section 4.2.2 of the spec.
  blk_intr.construct();
  net_intr.construct();
  net_devs.construct();
  demux.construct();

  for (const auto &[name, device] : devs) {
    auto base = device.base;
    // Erratic device.
    if (mmrd<int32_t>(base) != 0x74726976)
      continue;

    // We detect the format: either >= v1.0, or the legacy format.
    bool legacy = mmrd<int32_t>(base + 4) == 1;

    // We only care for block devices for now. See section 5.
    int device_id = mmrd<int32_t>(base + 8);

    if (device_id == 1) {
      auto net = new (os::permanent) net_device(device, legacy);

      plic::enable(device.interrupt);
      plic::record(device.interrupt, net_device_handler);

      (*net_intr)[device.interrupt] = net;
      net_devs->insert(net_device_cnt++, net);
      continue;
    }

    // Block device.
    if (device_id == 2) {
      auto dev = new (os::permanent) block_device(device, legacy);

      // Set up PLIC interrupt.
      plic::enable(device.interrupt);
      plic::record(device.interrupt, block_device_handler);

      // Create a node in devfs.
      char buf[4] = "vda";
      buf[2] += block_device_cnt++; // vda, vdb, vdc ...
      devnode->record(buf, new (os::permanent) block_inode(dev));

      (*blk_intr)[device.interrupt] = dev;
      continue;
    }
  }
}

net_device *netdev(int i) {
  [[unlikely]] if (!net_devs->size())
    panic("cannot find net device");
  return net_devs->at(i);
}

}
