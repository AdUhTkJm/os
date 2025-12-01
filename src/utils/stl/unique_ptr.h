#ifndef UNIQUE_PTR_H
#define UNIQUE_PTR_H

namespace os {

template<class T>
class unique_ptr {
  T *ptr;
public:
  unique_ptr(): ptr(nullptr) {}
  unique_ptr(T *ptr): ptr(ptr) {}
  unique_ptr(const unique_ptr<T>&) = delete;
  unique_ptr& operator=(const unique_ptr<T>&) = delete;

  unique_ptr(unique_ptr<T> &&other): ptr(other.ptr) {
    other.ptr = nullptr;
  }
  unique_ptr& operator=(unique_ptr<T>&& other) {
    if (this != &other) {
      delete ptr;
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }

  ~unique_ptr() { delete ptr; }

  T *get() { return ptr; }
  T &operator*() const { return *ptr; }
  T *operator->() const { return ptr; }
  T &operator[](unsigned long i) const { return ptr[i]; }
  operator bool() { return ptr; }

  void swap(unique_ptr &other) {
    T* tmp = ptr;
    ptr = other.ptr;
    other.ptr = tmp;
}

  void reset(T *p = nullptr) {
    delete ptr;
    ptr = p;
  }

  T* release() {
    T *p = ptr;
    ptr = nullptr;
    return p;
  }
};

// Everything is the same, just that `delete` is changed to `delete[]`.
template<class T>
class unique_ptr<T[]> {
  T *ptr;
public:
  unique_ptr(): ptr(nullptr) {}
  unique_ptr(T *ptr): ptr(ptr) {}
  unique_ptr(const unique_ptr<T>&) = delete;
  unique_ptr& operator=(const unique_ptr<T>&) = delete;

  unique_ptr(unique_ptr<T> &&other): ptr(other.ptr) {
    other.ptr = nullptr;
  }
  unique_ptr& operator=(unique_ptr<T>&& other) {
    if (this != &other) {
      delete ptr;
      ptr = other.ptr;
      other.ptr = nullptr;
    }
    return *this;
  }

  ~unique_ptr() { delete[] ptr; }

  T *get() { return ptr; }
  T &operator*() const { return *ptr; }
  T *operator->() const { return ptr; }
  T &operator[](unsigned long i) const { return ptr[i]; }
  operator bool() { return ptr; }

  void swap(unique_ptr &other) {
    T* tmp = ptr;
    ptr = other.ptr;
    other.ptr = tmp;
}

  void reset(T *p = nullptr) {
    delete ptr;
    ptr = p;
  }

  T* release() {
    T *p = ptr;
    ptr = nullptr;
    return p;
  }
};

}

#endif
