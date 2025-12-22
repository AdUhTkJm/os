#include "pipe.h"

namespace os {

// Change to printk for debugging.
#define CONCURRENCY_LOG(x) (void) 0

class pipefs pipefs;

pipe_inode::pipe_inode(os::fs *fs, int uid, int gid): inode_impl(fs, uid, gid, 0666, FIFO), maxbuf(pipefs::maxbuf) {}

size_t pipe_inode::read(size_t offset, void *buf, size_t len, int flags) {
  // Offset is not supported on pipes.
  (void) offset;
  lock.acquire();

  wait_entry entry;
  while (rpos == buffer.size()) {
    // No more writers. EOF.
    if (writers == 0) {
      lock.release();
      return 0;
    }

    if (flags & O_NONBLOCK) {
      lock.release();
      return -EAGAIN;
    }

    read_wait.prepare(entry);
    CONCURRENCY_LOG("read: suspend\n");
    lock.release();
    if (suspend() != 0)
      return -EINTR;
    lock.acquire();
    CONCURRENCY_LOG("read: resume\n");
    read_wait.finish(entry);
  }

  auto sz = buffer.size();
  auto l = min(buffer.size(), len);
  memcpy(buf, buffer.data() + rpos, l);
  rpos += l;

  // Resize the vector if it is large enough, and more than half of it is already read.
  bool freed = false;
  if (sz >= 1_kb && rpos >= sz / 2) {
    vector<char> v(sz - rpos);
    memcpy(v.data(), buffer.data() + rpos, sz - rpos);
    buffer = os::move(v);
    rpos = 0;
    freed = true;
  }
  lock.release();

  // Wake only on write_wait condition change.
  if (freed)
    CONCURRENCY_LOG("read: wake writers\n"), write_wait.wake();
  return l;
}

size_t pipe_inode::write(size_t offset, const void *buf, size_t len, int flags) {
  (void) offset;
  
  lock.acquire();

  wait_entry entry;
  bool empty = buffer.empty();
  while (buffer.size() == maxbuf) {
    // No more readers. Don't write.
    if (readers == 0) {
      lock.release();
      return -EPIPE;
    }

    if (flags & O_NONBLOCK) {
      lock.release();
      return -EAGAIN;
    }

    write_wait.prepare(entry);
    CONCURRENCY_LOG("write: suspend\n");
    lock.release();
    if (suspend() != 0)
      return -EINTR;
    lock.acquire();
    CONCURRENCY_LOG("write: resume\n");
    write_wait.finish(entry);
  }

  auto sz = buffer.size();
  auto l = min(maxbuf - sz, len);
  buffer.resize(sz + l);
  memcpy(buffer.data() + sz, buf, l);
  lock.release();

  // Wake only on read_wait condition change.
  if (empty)
    CONCURRENCY_LOG("write: wake readers\n"), read_wait.wake();
  return l;
}

short pipe_inode::poll(unsigned short event) {
  bool out = event & POLLOUT, in = event & POLLIN;
  auto result = 0;
  if (in && !buffer.empty())
    result |= POLLIN;
  if (out && buffer.size() < maxbuf)
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

  {
    synchronized _(lock);
    if (read)
      readers--;
    if (write)
      writers--;
  }

  if (!readers)
    CONCURRENCY_LOG("readers empty: wake writers\n"), write_wait.wake_all();
  if (!writers)
    CONCURRENCY_LOG("writers empty: wake readers\n"), read_wait.wake_all();
}

void pipe_inode::incf(const file *file) {
  if (can_read(file->flags))
    incread();
  if (can_write(file->flags))
    incwrite();
}

}
