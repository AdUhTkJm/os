#include "pipe.h"

namespace os {

// Change to printk for debugging.
static_storage<class pipefs> pipefs;

pipe_inode::pipe_inode(os::fs *fs, int uid, int gid):
  inode_impl(fs, uid, gid, 0666, FIFO), buffer((char *) as_va(pframe())), capacity(4096) {}

ssize_t pipe_inode::read(size_t, void *buf, size_t len, int flags) {
  lock.acquire();
  wait_entry entry;

  while (available() == 0) {
    // No more writers, so EOF.
    if (writers == 0) {
      lock.release();
      return 0;
    }
    if (flags & O_NONBLOCK) {
      lock.release();
      return -EAGAIN;
    }
    hangon(read_wait, lock, entry);
  }

  size_t l = min(len, available());
  char *dst = (char *) buf;

  size_t chunk = min(l, capacity - index(rpos));
  memcpy(dst, buffer + index(rpos), chunk);
  if (chunk < l)
    memcpy(dst + chunk, buffer, l - chunk);

  rpos += l;
  lock.release();
  write_wait.wake();
  return l;
}

ssize_t pipe_inode::write(size_t, const void *buf, size_t len, int flags) {
  lock.acquire();
  if (readers == 0) {
    lock.release();
    active()->send_signal(SIGPIPE);
    return -EPIPE;
  }
  if (space() == 0 && (flags & O_NONBLOCK)) {
    lock.release();
    return -EAGAIN;
  }

  wait_entry entry;
  while (space() == 0) {
    hangon(write_wait, lock, entry);
  }

  size_t l = min(len, space());
  const char *src = (const char *) buf;

  size_t chunk = min(l, capacity - index(wpos));
  memcpy(buffer + index(wpos), src, chunk);
  if (chunk < l)
    memcpy(buffer, src + chunk, l - chunk);

  wpos += l;
  lock.release();
  read_wait.wake();
  return l;
}

short pipe_inode::poll(unsigned short event) {
  bool out = event & POLLOUT, in = event & POLLIN;
  auto result = 0;
  if (in && (available() != 0 || writers == 0))
    result |= POLLIN;
  if (out && (space() != 0 || readers == 0))
    result |= POLLOUT;
  return result;
}

void pipe_inode::prepare_read_wait(wait_entry &entry) {
  read_wait.prepare(entry);
}

void pipe_inode::prepare_write_wait(wait_entry &entry) {
  write_wait.prepare(entry);
}

void pipe_inode::finish_read_wait(wait_entry &entry) {
  read_wait.finish(entry);
}

void pipe_inode::finish_write_wait(wait_entry &entry) {
  write_wait.finish(entry);
}

void pipe_inode::onclose(int flags) {
  bool read = (flags & 0x3) == O_RDONLY || (flags & 0x3) == O_RDWR;
  bool write = (flags & 0x3) == O_WRONLY || (flags & 0x3) == O_RDWR;

  lock.acquire();
  if (read && !--readers)
    write_wait.wake_all();
  
  if (write && !--writers)
    read_wait.wake_all();
  
  lock.release();
  scheduler.maybe_preempt();
}

void pipe_inode::incf(const file *file) {
  if (file->readable())
    incread();
  if (file->writable())
    incwrite();
}

}
