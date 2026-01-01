#ifndef RBTREE_H
#define RBTREE_H

// I have no idea how to manipulate a Red-Black tree, even though it is taught in Algorithms 2 course (very briefly).
// This is mainly by AI.

#include "utility.h"

namespace os {

enum color { Red, Black };

template<typename K, typename V>
struct rb_node {
  K key;
  enum color color = Red;
  V *parent = nullptr;
  V *l = nullptr, *r = nullptr;
};

template <typename K, typename V>
class rb_tree {
  void rotate_left(V *x) {
    V *y = x->r;
    x->r = y->l;
    if (y->l)
      y->l->parent = x;
    y->parent = x->parent;
    if (!x->parent)
      root = y;
    else if (x == x->parent->l)
      x->parent->l = y;
    else
      x->parent->r = y;
    y->l = x;
    x->parent = y;
  }

  void rotate_right(V *y) {
    V *x = y->l;
    y->l = x->r;
    if (x->r)
      x->r->parent = y;
    x->parent = y->parent;
    if (!y->parent)
      root = x;
    else if (y == y->parent->l)
      y->parent->l = x;
    else
      y->parent->r = x;
    x->r = y;
    y->parent = x;
  }

  void insert_fixup(V *z) {
    while (z->parent && z->parent->color == Red) {
      if (z->parent == z->parent->parent->l) {
        V *y = z->parent->parent->r; // The "Uncle"
        if (y && y->color == Red) {
          // Case 1: Uncle is Red -> Recolor
          z->parent->color = Black;
          y->color = Black;
          z->parent->parent->color = Red;
          z = z->parent->parent;
        } else {
          // Case 2 & 3: Uncle is Black -> Rotate
          if (z == z->parent->r) {
            z = z->parent;
            rotate_left(z);
          }
          z->parent->color = Black;
          z->parent->parent->color = Red;
          rotate_right(z->parent->parent);
        }
      } else {
        // Symmetric case: parent is the right child
        V *y = z->parent->parent->l;
        if (y && y->color == Red) {
          z->parent->color = Black;
          y->color = Black;
          z->parent->parent->color = Red;
          z = z->parent->parent;
        } else {
          if (z == z->parent->l) {
            z = z->parent;
            rotate_right(z);
          }
          z->parent->color = Black;
          z->parent->parent->color = Red;
          rotate_left(z->parent->parent);
        }
      }
    }
    root->color = Black;
  }
public:
  V *root = nullptr;
  
  V *find(K key) const {
    for (V *cur = root; cur; cur = (key < cur->key) ? cur->l : cur->r) {
      if (key == cur->key)
        return cur;
    }
    return nullptr;
  }

  void insert(V *z) {
    V *y = nullptr;
    V *x = root;

    while (x) {
      y = x;
      assert(z->key != x->key);
      if (z->key < x->key)
        x = x->l;
      else
        x = x->r;
    }

    z->parent = y;
    if (!y)
      root = z;
    else if (z->key < y->key)
      y->l = z;
    else
      y->r = z;

    z->l = z->r = nullptr;
    z->color = Red;
    insert_fixup(z);
  }
};

}

#endif
