#ifndef LIST_H
#define LIST_H

#include "../helper_meta.h"
#include "utility.h"
#include <stddef.h>

namespace os {

template<class T>
struct intrusive_list_node {
  T *prev, *next;
  ~intrusive_list_node() = default;
};

template<class T>
concept intrusive_capable = is_base_of<intrusive_list_node<T>, T>::value;

template<intrusive_capable T>
class intrusive_list {
  T *head = nullptr, *tail = nullptr;
  size_t sz = 0;

  intrusive_list_node<T> *into(T *node) const {
    return static_cast<intrusive_list_node<T> *>(node);
  }
  friend class iterator;
public:
  class iterator {
  public:
    T *node;
    intrusive_list<T> &parent;
    iterator(intrusive_list<T> &parent, T *p): node(p), parent(parent) {}

    iterator &operator++() {
      if (!node)
        return *this;

      if (node == parent.tail)
        node = nullptr;
      else
        node = node->next;
      return *this;
    }

    iterator &operator--() {
      if (!node)
        node = parent.tail;
      else
        node = node->prev;
      return *this;
    }

    T *operator*() {
      return node;
    }

    bool operator==(const iterator &other) const { return node == other.node; }
    bool operator!=(const iterator &other) const { return node != other.node; }
  };

  bool empty() const { return !head; }

  // Note: this won't work if `node` is inside the list.
  void push_back(T* node) {
    intrusive_list_node<T> *link = into(node);
    link->prev = tail;
    link->next = nullptr;

    if (tail)
      into(tail)->next = node;
    else
      head = node;
    tail = node;
    sz++;
  }

  void pop_front() {
    if (!head)
      return;

    T *front = head;
    intrusive_list_node<T> *link = into(front);

    head = link->next;
    if (head)
      into(head)->prev = nullptr;
    else
      tail = nullptr;
    
    link->prev = link->next = nullptr;
    sz--;
  }

  // This won't work if node is not inside the list.
  void erase(T* node) {
    intrusive_list_node<T> *link = into(node);

    if (link->prev)
      into(link->prev)->next = link->next;
    else
      head = link->next;

    if (link->next)
      into(link->next)->prev = link->prev;
    else
      tail = link->prev;
    
    link->prev = link->next = nullptr;
    sz--;
  }

  T *&back() { return tail; }
  T *&front() { return head; }
  T *back() const { return tail; }
  T *front() const { return head; }

  iterator begin() { return iterator(*this, head); }
  iterator end() { return iterator(*this, nullptr); }
  size_t size() const { return sz; }
};

template<class T>
class list {
  struct node {
    T data;
    node *next, *prev;
  };

  node *head = nullptr, *tail = nullptr;
  size_t sz = 0;
public:
  class iterator {
    node *n;
    friend list;
  public:
    iterator(node *n): n(n) {}

    iterator &operator++() {
      if (n)
        n = n->next;
      return *this;
    }

    T *operator->() const {
      return n->data;
    }

    T &operator*() const {
      return n->data;
    }

    bool operator==(const iterator &other) const {
      return n == other.n;
    }
  };

  ~list() {
    for (node *p = head; p != tail;) {
      auto next = p->next;
      delete p;
      p = next;
    }
  }

  void push_back(const T &item) {
    node *n = new (safe) node { item, nullptr, nullptr };
    sz++;
    if (!tail) {
      head = tail = n;
      return;
    }
    tail->next = n;
    n->prev = tail;
    tail = n;
  }

  void pop_front() {
    if (!head)
      return;
    node *n = head;
    head = head->next;
    if (!head)
      tail = nullptr;
    sz--;
    delete n;
  }

  void erase(iterator it) {
    node *t = it.n;
    if (t == head)
      head = t->next;
    else
      t->prev->next = t->next;
    if (t == tail)
      tail = t->prev;
    else
      t->next->prev = t->prev;
  }

  T front() { return head->data; }
  T back() { return tail->data; }

  iterator begin() { return head; }
  iterator end() { return nullptr; }

  bool empty() const { return sz == 0; }
  size_t size() const { return sz; }
};

}

#endif

