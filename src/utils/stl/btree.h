#ifndef BTREE_H
#define BTREE_H

#include "utility.h"

// This file is actually written after the b-tree in `vma.h`.
// We delete the specific logic of updating maxgap or maxend here.

namespace os {

template<class K, class V, int Order> requires(Order % 2 == 0)
class btree {
  int sz = 0;

  struct node {
    // Remember that #children = #keys + 1. Here `count` is the number of keys.
    // The maximum number of keys is Order - 1, while the minimum number of keys is ceil(Order / 2) - 1.

    node *ch[Order];    // Children.
    K k[Order - 1];     // Keys.
    V v[Order - 1];     // Values.
    int count = 0;
    bool leaf;

    node(bool leaf): leaf(leaf) {}
    // The minimum begin in children. No extra maintaining; it is already sorted.
    K minimal() {
      if (leaf)
        return k[0];
      return ch[0]->minimal();
    }
  } *root = nullptr;
  
  // The minimum number of keys is t - 1, and the minimum number of children is t.
  static constexpr int t = Order / 2;
  
  // Actually, this works exactly like a sorted tree;
  // Just that each time we pick the rightmost (largest) child.
  //
  // These don't handle the case for leaves.
  // They're only called in `erase`, which ensures that they aren't leaves.
  node *pred(node *x, int i) {
    x = x->ch[i];           // Go left once;
    while (!x->leaf)
      x = x->ch[x->count];  // Then always go right.
    return x;
  }

  // By symmetry to pred().
  node *succ(node *x, int i) {
    x = x->ch[i + 1];       // Go right once;
    while (!x->leaf)
      x = x->ch[0];         // Then always go left.
    return x;
  }

  // Now `x` is full, so it has M - 1 keys.
  // Moves half of `x`'s keys into parent `p`.
  void split(node *p, int i, node *x) {
    // Create a new node `z` and move half of keys to it.
    node *z = new node(x->leaf);

    // Remember that t = M / 2, where M is the number of children.
    // After split, both `x` and `z` should have `t` children, and hence `t-1` keys.

    // The keys [t, 2t - 1) will be given to `z`.
    z->count = t - 1;
    for (int j = 0; j < t - 1; j++) {
      z->k[j] = x->k[j + t];
      z->v[j] = x->v[j + t];
    }

    // The children [t, 2t) will be given to `z`.
    if (!x->leaf) {
      for (int j = 0; j < t; j++)
        z->ch[j] = x->ch[j + t];
    }

    x->count = t - 1;

    // Now the middle element [t - 1] goes into parent `p`.
    // (Note we actually have `2t - 1` keys, and hence `t - 1` is the middle.)
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

  // Fill the keys of x->ch[i], so that it has enough elements (>= Order / 2 + 1) for deletion.
  void fill(node *x, int i) {
    // The left child has a spare key.
    // We're taking the rightmost key of left child into this node,
    // and take the key of this node into the right child (target child).
    if (i > 0 && x->ch[i - 1]->count >= t) {
      node *l = x->ch[i - 1];
      node *r = x->ch[i];

      // Shift all keys, values and children right to spare an empty slot in right child.
      for (int j = r->count; j >= 1; j--) {
        r->k[j] = r->k[j - 1];
        r->v[j] = r->v[j - 1];
      }
      if (!r->leaf) {
        for (int j = r->count + 1; j >= 1; j--)
          r->ch[j] = r->ch[j - 1];
      }

      // Assign this node's key to right child.
      r->k[0] = x->k[i - 1];
      r->v[0] = x->v[i - 1];
      if (!r->leaf)
        r->ch[0] = l->ch[l->count];

      // Assign left child's last key to this node.
      x->k[i - 1] = l->k[l->count - 1];
      x->v[i - 1] = l->v[l->count - 1];

      l->count--;
      r->count++;
      return;
    }
    
    // In symmetry, we can take a key from right child.
    if (i < x->count && x->ch[i + 1]->count >= t) {
      node *l = x->ch[i];
      node *r = x->ch[i + 1];

      // Assign this node's key to left child.
      l->k[l->count] = x->k[i];
      l->v[l->count] = x->v[i];
      if (!r->leaf)
        l->ch[l->count + 1] = r->ch[0];

      // Assign right child's first key to this node.
      x->k[i] = r->k[0];
      x->v[i] = r->v[0];

      // Shift all keys, values and children to the left in the right child.
      for (int i = 0; i < r->count - 1; i++) {
        r->k[i] = r->k[i + 1];
        r->v[i] = r->v[i + 1];
      }
      if (!r->leaf) {
        for (int i = 0; i < r->count; i++)
          r->ch[i] = r->ch[i + 1];
      }

      l->count++;
      r->count--;
      return;
    }
    
    // Neither of left and right sibling have enough keys.
    // In this case, merging them will never exceed the capacity (Order - 1).
    merge(x, i < x->count ? i : i - 1);
  }

  void insert_impl(node *x, K key, const V &value) {
    int i = x->count - 1;
    while (i >= 0 && key < x->k[i])
      i--;
    if (i >= 0 && x->k[i] == key) {
      x->v[i] = value;
      return;
    }
    i++;

    if (x->leaf) {
      // Do a linear scan of the keys to find the insert position.
      // No need to binary search, we must move them anyway.
      for (int j = x->count - 1; j >= i; j--) {
        x->k[j + 1] = x->k[j];
        x->v[j + 1] = x->v[j];
      }
      x->k[i] = key;
      x->v[i] = value;
      // We've guaranteed this won't overflow, since when we descend,
      // we'll split children as needed.
      x->count++;
    } else {
      // Found a full child, split it to continue.
      if (x->ch[i]->count == 2 * t - 1) {
        split(x, i, x->ch[i]);
        // If we end up in the right child (newly split one),
        // we must move `i` to the right.
        if (x->k[i] < key)
          i++;
        // If we just moved up the median here, then we must change it and not descend.
        else if (x->k[i] == key) {
          x->v[i] = value;
          return;
        }
      }
      insert_impl(x->ch[i], key, value);
    }
  }

  // Merge p->k[i] and its two children, p->k[i] and p->k[i + 1].
  void merge(node *p, int i) {
    // The merge target is `l`.
    node *l = p->ch[i];
    node *r = p->ch[i + 1];

    // Copy p's data into `l`.
    assert(l->count == t - 1 && r->count == t - 1);
    l->k[t - 1] = p->k[i];
    l->v[t - 1] = p->v[i];

    // Copy r's data into `l`.
    for (int j = 0; j < t - 1; ++j) {
      l->k[j + t] = r->k[j];
      l->v[j + t] = r->v[j];
    }
    if (!l->leaf) {
      for (int j = 0; j < t; ++j)
        l->ch[j + t] = r->ch[j];
    }
    l->count = 2 * t - 1;

    // Remove the key from parent.
    for (int j = i; j < p->count - 1; j++) {
      p->k[j] = p->k[j + 1];
      p->v[j] = p->v[j + 1];
    }
    for (int j = i + 1; j < p->count; j++)
      p->ch[j] = p->ch[j + 1];
    p->count--;
    
    delete r;
  }

  void erase_impl(node *n, K key) {
    int i = 0;
    while (i < n->count && n->k[i] < key)
      i++;

    // The key is in this node. Directly erase it.
    if (i < n->count && n->k[i] == key) {
      if (n->leaf) {
        for (auto j = i; j < n->count - 1; j++) {
          n->k[j] = n->k[j + 1];
          n->v[j] = n->v[j + 1];
        }
        n->count--;
        sz--;
        return;
      }
      
      // When the node has children, we must make sure they are well-handled.
      node *l = n->ch[i];
      node *r = n->ch[i + 1];

      // We use the classic "swap-and-delete" technique:
      // We move the smallest element greater than `key`, or the largest one smaller than `key`,
      // into the position we want to delete.
      // Afterwards, we only need to delete the now-duplicate key in the subtree.
      if (l->count >= t) {
        auto p = pred(n, i);
        n->k[i] = p->k[p->count - 1];
        n->v[i] = p->v[p->count - 1];
        erase_impl(l, n->k[i]);
      } else if (r->count >= t) {
        auto s = succ(n, i);
        n->k[i] = s->k[0];
        n->v[i] = s->v[0];
        erase_impl(r, n->k[i]);
      } else {
        K key = n->k[i];
        merge(n, i);
        erase_impl(l, key);
      }
      return;
    }

    // Key not found.
    if (n->leaf)
      return;

    // The key is in a subtree.
    // We must make sure this subtree has at least `t` nodes, since
    // after deleting it must have at least `t - 1`.
    if (n->ch[i]->count == t - 1)
      fill(n, i);

    // fill() might have changed this node, especially on merge.
    // To avoid complicated reasoning, we just re-search the key.
    for (i = 0; i < n->count && n->k[i] < key; i++);
    erase_impl(n->ch[i], key);
  }

  void clear(node *x) {
    if (!x)
      return;
    if (!x->leaf) {
      for (int i = 0; i <= x->count; i++)
        clear(x->ch[i]);
    }
    delete x;
  }
public:
  constexpr static int order = Order;
  using key_type = K;
  using value_type = V;

  class iterator {
    constexpr static int stack_size = 8;
    struct stack_frame {
      node *n;
      int i;
    } stack[stack_size];
    // Use -1 for end.
    int top = -1;

    node *mynode() { return stack[top].n; }
    int &index() { return stack[top].i; }

    friend class btree;
  public:
    bool operator==(const iterator& other) const {
      if (top != other.top)
        return false;
      if (top == -1)
        return true;
      return stack[top].n == other.stack[top].n && stack[top].i == other.stack[top].i;
    }

    iterator &operator++() {
      // Don't increment end() pointer.
      if (top == -1)
        return *this;

      if (!mynode()->leaf) {
        // Similar to succ(), but we're recording the path here.
        node *next = mynode()->ch[++index()];
        while (next) {
          stack[++top] = { next, 0 };
          if (next->leaf)
            break;
          next = next->ch[0];
        }
        return *this;
      }

      // Move to the next key in this leaf.
      // If we exhausted this node, climb up the stack.
      index()++;

      // Don't forget that `index()` and `mynode()` changes with `top`.
      while (top >= 0 && index() >= mynode()->count) {
        if (--top >= 0 && index() < mynode()->count)
          return *this;
      }
      return *this;
    }

    pair<K, V&> operator*() {
      return { mynode()->k[index()], mynode()->v[index()] };
    }
  };

  iterator begin() {
    iterator it;
    if (!root) {
      it.top = -1;
      return it;
    }

    // This is just the leftmost child, but we must record the path.
    node *curr = root;
    while (curr) {
      // We try to avoid dynamic allocation.
      assert(it.top < iterator::stack_size);
      it.stack[++it.top] = { curr, 0 };
      if (curr->leaf)
        break;
      
      curr = curr->ch[0];
    }
    return it;
  }

  iterator end() { return iterator(); }

  btree() {}

  ~btree() {
    // Delete the entire tree.
    clear();
  }

  void insert(K key, const V &value) {
    sz++;
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
      s->ch[0] = root;
      split(s, 0, root);
      root = s;
    }
    
    insert_impl(root, key, value);
  }

  void erase(K key) {
    if (!root)
      return;
    erase_impl(root, key);

    // If the root has no keys left, it means all its data 
    // was moved into its first child during a merge.
    if (root->count == 0) {
      node *old = root;
      // The tree is completely empty.
      if (root->leaf)
        root = nullptr;
      else
        // The first (and only) child becomes the new root.
        root = root->ch[0];
      
      delete old;
    }
  }

  void clear() {
    clear(root);
    root = nullptr;
    sz = 0;
  }

  V *find(const K &key) {
    node *curr = root;

    for (node *x = root; x;) {
      int i = 0;
      while (i < x->count && x->k[i] < key)
        i++;

      if (i < x->count && x->k[i] == key)
        return &x->v[i];

      if (x->leaf)
        return nullptr;

      x = x->ch[i];
    }

    return nullptr;
  }

  int size() { return sz; }
};

}

#endif
