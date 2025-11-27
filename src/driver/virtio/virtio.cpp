#include "virtio.h"
#include "../../fdt/fdt.h"
#include "../../mem/kalloc.h"
#include "../../proc/schedule.h"

namespace {

uint32_t read(uint32_t *p) {
  return to_big_endian(*p);
}

int block_device_cnt = 0;

}

namespace os::virtio {

static_storage<os::hashmap<int, block_device*>> disks;

// Configures the underlying device.
// See section 3.1.1.
block_device::block_device(const device &device, bool legacy): descid(0) {
  base = device.base;
  this->legacy = legacy;

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

  mmwr(base + DEVICE_FEATURESEL, 1);
  auto features_high = mmrd<uint32_t>(base + DEVICE_FEATURE);
  // Legacy should be indicated by `VERSION_1` not offered.
  assert(bool(features_high & (((uint64_t) features::VERSION_1) >> 32)) ^ legacy);

  mmwr(base + DRIVER_FEATURESEL, 1);
  mmwr<uint32_t>(base + DRIVER_FEATURE, supported >> 32);

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
  if (mmrd<int32_t>(base + QUEUE_READY) != 0)
    panic("queue occupied");

  if (legacy && mmrd<int32_t>(base + QUEUE_PFN) != 0)
    panic("queue occupied");

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
    queue = new vq::queue {
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

    // Allocate space for buffers.
    req = pmalloc(1);
    buffer = pmalloc(1);
    stat = pmalloc(1);

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
  assert(legacy);
  auto queue = (vq::queue_legacy*) this->queue;
  auto &desc = queue->desc[descid];
  descid = (descid + 1) % vq::size;
  return desc;
}

uint16_t block_device::indexof(const vq::desc &desc) {
  assert(legacy);
  auto queue = (vq::queue_legacy*) this->queue;
  return &desc - queue->desc;
}

// The lba is the logical block address.
result block_device::read_legacy(uint64_t lba, void *buffer) {
  // We have to follow the layout specified by 5.2.6.4 for legacy drivers.
  *(request_legacy *) as_va(req) = request_legacy {
    .type = 0, /* Read */
    .reserved = 0,
    .sector = lba,
  };
  
  mmwr(stat, uint8_t(0xff));

  vq::desc &d1 = next_descriptor();
  vq::desc &d2 = next_descriptor();
  vq::desc &d3 = next_descriptor();
  d1.addr = req;
  d1.len = sizeof(request_legacy);
  d1.flags = vq::descflags::HAS_NEXT;
  d1.next = indexof(d2);

  d2.addr = this->buffer;
  d2.len = 512;
  d2.flags = vq::descflags::HAS_NEXT | vq::descflags::WRITEONLY;
  d2.next = indexof(d3);

  d3.addr = stat;
  d3.len = 1;
  d3.flags = vq::descflags::WRITEONLY;
  d3.next = 0;

  // Put the head of chain into available ring for the device to read.
  auto queue = (vq::queue_legacy*) this->queue;
  queue->avail.ring[queue->avail.idx % vq::size] = indexof(d1);
  queue->avail.idx++;

  // Tell device that a new request has come.
  __asm__ volatile ("fence w,rw" ::: "memory");
  mmwr(base + QUEUE_NOTIFY, /*queue_index=*/0);
  
  // Spinwait when there is no current process.
  if (scheduler.active->pid == -1) {
    volatile int timeout = 10000000;
    while (mmrd<uint8_t>(stat) == 0xff) {
      if (--timeout == 0) {
        printk("Error: VirtIO request timed out.\n");
        return result::failure;
      }
    }
  }

  auto status = mmrd<uint8_t>(stat);
  if (status == 0) {
    // hexdump((void *) as_va(this->buffer), 512);
    memcpy(buffer, (void*) as_va(this->buffer), 512);
    return result::success;
  }

  // Status 1 = error, 2 = unsupported
  printk("failure: %d\n", status);
  return result::failure;
}

result block_device::read(uint64_t lba, void *buffer) {
  if (legacy)
    return read_legacy(lba, buffer);
  assert(false && "NYI for non-legacy devices");
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

    if (strcmp(cprop, "interrupt") == 0)
      devs[cdev].interrupt = to_big_endian(*(uint32_t*) property);

    return WalkResult::Continue;
  });

  // We only consider mmio devices for now.
  // See section 4.2.2 of the spec.
  disks.construct();
  for (const auto &[name, device] : devs) {
    auto base = device.base;
    // Erratic device.
    if (mmrd<int32_t>(base) != 0x74726976)
      continue;

    // We detect the format: either >= v1.0, or the legacy format.
    bool legacy = mmrd<int32_t>(base + 4) == 1;

    // We only care for block devices for now. See section 5.
    int device_id = mmrd<int32_t>(base + 8);
    if (device_id != 2)
      continue;

    auto dev = new block_device(device, legacy);
    (*disks)[block_device_cnt++] = dev;
  }
}

block_device *get(int id) {
  if (!disks->count(id))
    return nullptr;

  return disks->at(id);
}

}
