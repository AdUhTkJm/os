#ifndef BTREE_H
#define BTREE_H

#include "../helper_meta.h"
#include "hash.h"

namespace os {

template<class T>
struct less {
  bool operator()(const T& a, const T& b) const {
    return a < b;
  }
};

// See https://www.cl.cam.ac.uk/teaching/2324/Algorithm1/content/slides22.pdf
// Cambridge Algorithm 2 course, Part IA.
template<class K, class V, int Order, comparator<K> Less = less<K>>
class btree {
  struct node {
    node *ch[Order - 1] {}; // Childrens.
    K k[Order - 1]; // Keys.
    V v[Order - 1]; // Values.
    int count = 0;
    bool leaf;

    node(bool leaf): leaf(leaf) {}
  } *root = nullptr;
  Less lt;

  // Now `x` is full, so it has M - 1 keys.
  // Moves half of `x`'s keys into parent `p`.
  void split(node *p, int i, node *x) {
    int t = Order / 2;
    node *z = new node(x->leaf);
    // Create a new node `z` and move half of keys to it.
    z->count = t - 1;
    x->count = t - 1;

    for (int j = 0; j < t - 1; j++) {
      z->k[j] = os::move(x->k[j + t]);
      z->v[j] = os::move(x->v[j + t]);
    }

    // Now children [t, 2t), i.e. [M/2, M-1], should attach to `z`.
    if (!x->leaf) {
      for (int j = 0; j < t; j++)
        z->ch[j] = x->ch[j + t];
    }

    // Now the middle element goes into parent `p`.
    // We must move everything to the right.
    for (int j = p->count - 1; j >= i; j--) {
      p->k[j + 1] = p->k[j];
      p->v[j + 1] = p->v[j];
    }

    p->k[i] = x->k[t - 1];
    p->v[i] = x->v[t - 1];

    // `x` remains in place, but to the right of it we have a new children `z`.
    for (int j = p->count; j >= i + 1; j--)
      p->ch[j + 1] = p->ch[j];
    p->ch[i + 1] = z;
    p->count++;
  }

  void insert_impl(node* x, K key, V value) {
    int i = x->count - 1;

    if (x->leaf) {
      // Do a linear scan of the keys to find the insert position.
      // No need to binary search, we must move them anyway.
      while (i >= 0 && lt(key, x->k[i])) {
        x->k[i + 1] = x->k[i];
        x->v[i + 1] = x->v[i];
        i--;
      }
      x->k[i + 1] = key;
      x->v[i + 1] = value;
      // We've guaranteed this won't overflow, since when we descend,
      // we'll split children as needed.
      x->count++;
    } else {
      // Perhaps we can binary search this one?
      while (i >= 0 && lt(key, x->k[i]))
        i--;
      i++;

      // Found a full child, split it to continue.
      if (x->ch[i]->count == Order - 1) {
        split(x, i, x->ch[i]);
        // If we end up in the right child (newly split one),
        // we must move `i` to the right.
        if (lt(x->k[i], key))
          i++;
      }
      insert_impl(x->ch[i], key, value);
    }
  }
public:
  constexpr static int order = Order;

  void insert(K key, V value) {
    if (!root) {
      root = new node(true);
      root->k[0] = key;
      root->v[0] = value;
      root->count = 1;
      return;
    }

    // The root is full. We must replace it.
    // This is like manually unroll insert_impl(), non-leaf case, where x->count == 1.
    if (root->count == Order - 1) {
      node *s = new node(false);
      s->children[0] = root;
      split(s, 0, root);
      
      int i = lt(s->k[0], key);
      insert_impl(s->ch[i], key, value);
      root = s;
      return;
    }
    
    insert_impl(root, key, value);
  }
};

}

#endif
