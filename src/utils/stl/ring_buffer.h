#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include "utility.h"
#include "../../lock/mutex.h"
#include "optional.h"

namespace os {

// This is a queue.
template<class T, int Size = 1024 / sizeof(T)>
class ring_buffer {
  T ring[Size];
  int head = 0, tail = 0, count = 0;
  mutex lock;
  condvar onempty, onfull;

  template<class Blk>
  using pop_ret_t = conditional_t<is_same_v<Blk, block>, T, optional<T>>;
public:
  ring_buffer(const ring_buffer&) = delete;
  ring_buffer &operator=(const ring_buffer&) = delete;
  ring_buffer(): onempty(lock), onfull(lock) {}

  constexpr static size_t capacity = Size;

  template<blockspec Blk> // noblock
  Blk push_back(const T &item) {
    synchronized syn(lock);
    if constexpr (is_same_v<Blk, block>) {
      while (count == Size)
        onfull.wait();
    } else {
      if (count == Size)
        return false;
    }
    
    ring[tail] = item;
    tail = (tail + 1) % Size;
    count++;
    onempty.notify();

    constexpr Blk v = Blk();
    if constexpr (is_same_v<Blk, block>)
      return v;
    else  
      return !v;
  }

  template<blockspec Blk> // noblock
  pop_ret_t<Blk> pop_front() {
    synchronized syn(lock);
    if constexpr (is_same_v<Blk, block>) {
      while (count == 0)
        onempty.wait();
    } else {
      if (count == 0)
        return os::nullopt;
    }
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

}

#endif
