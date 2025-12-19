#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "utility.h"
#include "../../lock/mutex.h"
#include "optional.h"

namespace os {

// This is a thread-safe queue.
template<class T, int Size = 1024 / sizeof(T), blockspec Blk = noblock>
class ring_buffer {
  T ring[Size];
  int head = 0, tail = 0, count = 0;
  spinlock lock;

public:
  ring_buffer(const ring_buffer&) = delete;
  ring_buffer &operator=(const ring_buffer&) = delete;
  ring_buffer() {}

  constexpr static size_t capacity = Size;

  bool push_back(const T &item) {
    synchronized syn(lock);
    if (count == Size)
      return false;
    
    ring[tail] = item;
    tail = (tail + 1) % Size;
    count++;

    constexpr Blk v = Blk();
    if constexpr (is_same_v<Blk, block>)
      return v;
    else  
      return !v;
  }

  optional<T> pop_front() {
    synchronized syn(lock);
    if (count == 0)
      return os::nullopt;
    
    auto item = ring[head];
    head = (head + 1) % Size;
    count--;
    
    return item;
  }

  bool empty() {
    synchronized syn(lock);
    return count == 0;
  }

  bool full() {
    synchronized syn(lock);
    return count == Size;
  }

  size_t size() {
    synchronized syn(lock);
    return count;
  }
};

template<class T, int Size>
class ring_buffer<T, Size, block> {
  T ring[Size];
  int head = 0, tail = 0, count = 0;
  mutex lock;
  condvar onempty, onfull;

public:
  ring_buffer(const ring_buffer&) = delete;
  ring_buffer &operator=(const ring_buffer&) = delete;
  ring_buffer() {}

  constexpr static size_t capacity = Size;

  void push_back(const T &item) {
    synchronized syn(lock);
    while (count == Size)
      onfull.wait(lock);
    ring[tail] = item;
    tail = (tail + 1) % Size;
    count++;
    onfull.notify();
  }

  T pop_front() {
    synchronized syn(lock);
    while (count == 0)
      onempty.wait(lock);
    auto item = ring[head];
    head = (head + 1) % Size;
    count--;
    onfull.notify();
    return item;
  }

  bool empty() {
    synchronized syn(lock);
    return count == 0;
  }

  bool full() {
    synchronized syn(lock);
    return count == Size;
  }

  size_t size() {
    synchronized syn(lock);
    return count;
  }
};

template<class T, size_t Size = 1024 / sizeof(T), blockspec Blk = noblock>
using static_queue = ring_buffer<T, Size, Blk>;

}

#endif
