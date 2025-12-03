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

  intrusive_list_node<T> *into(T *node) {
    return static_cast<intrusive_list_node<T> *>(node);
  }
public:
  using iterator = intrusive_list_node<T>*;

  bool empty() const { return !head; }

  // Note: this won't work if `node` is inside the list.
  void push_back(T* node) {
    iterator link = into(node);
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

    T* front = head;
    iterator link = into(front);

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
    iterator link = into(node);

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

  T &back() { return *tail; }
  T &front() { return *head; }
  const T &back() const { return *tail; }
  const T &front() const { return *head; }

  T *begin() { return head; }
  T *end() { return nullptr; }
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

  T front() { return head->data; }
  T back() { return tail->data; }

  bool empty() const { return sz == 0; }
  size_t size() const { return sz; }
};

}

#endif

