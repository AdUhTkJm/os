#ifndef VMA_H
#define VMA_H

#include "../utils/helper.h"
#include "../fs/vfs.h"
#include "ptable.h"

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

// Kernel internal usage, used for brk().
#define VMA_IS_HEAP    0x40
#define VMA_IS_STACK   0x80
#define VMA_IS_PT_LOAD 0x100

namespace os {

template<class T>
struct less {
  bool operator()(const T& a, const T& b) const {
    return a < b;
  }
};

}

namespace os::vma {

struct vma_t {
  uintptr_t begin, end;
  int prot, flags;
  file *backup;
  size_t offset, maxread;

  vma_t(): backup(nullptr) {}
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags);
  vma_t(uintptr_t begin, uintptr_t end, int prot, int flags, file *backup, size_t offset, size_t maxread);
  vma_t(const vma_t &other);
  vma_t(vma_t &&other);
  ~vma_t();

  vma_t &operator=(const vma_t &other);

  bool mergeable(const vma_t &other) const;
};

// See https://www.cl.cam.ac.uk/teaching/2324/Algorithm1/content/slides22.pdf
// Cambridge Algorithm 2 course, Part IA.
// We specialized it for the need of VMA.
template<int Order>
class btree {
  using K = va_t;
  using V = vma_t;

  struct node {
    // Remember that #children = #keys + 1. Here `count` is the number of keys.

    node *ch[Order] {}; // Children.
    K k[Order - 1];     // Keys.
    V v[Order - 1];     // Values.
    size_t maxend;      // Max ending point in children.
    size_t maxgap;      // Max gap in children.
    int count = 0;
    bool leaf;

    node(bool leaf): leaf(leaf) {}
    // The minimum begin in children. No extra maintaining; it is already sorted.
    void begin() {
      if (leaf)
        return k[0];
      return ch[0]->begin();
    }
  } *root = nullptr;

  // Updates the `max` values, whenever a node itself or its children changes.
  // We don't need to be recursive, as all children are already up to date.
  void update_impl(node *n) {
    if (!n)
      return;

    size_t end = 0, gap = 0;
    
    for (int i = 0; i < n->count; i++) {
      end = max(end, n->v[i].end);
      if (n->leaf && i > 0)
        n->maxgap = max(n->maxgap, n->k[i] - n->v[i - 1].end);
      

      if (!n->leaf) {
        end = max(end, n->ch[i]->maxend);
        // We know `ch[i]` is left of `k[i]`, and therefore its end might not reach `k[i]`'s end.
        // There might be a gap, and we calculate its length.
        gap = max(gap, max(n->ch[i]->maxgap, n->k[i] - n->ch[i]->maxend));
       // Similarly, we calculate the length of the right node here.
        gap = max(gap, n->ch[i + 1]->begin() - n->v[i].end);
      }
    }
    
    // We didn't update the last child in the previous loop; do it here.
    if (!n->leaf) {
      end = max(end, n->ch[n->count]->maxend);
      gap = max(gap, n->ch[n->count]->maxgap);
    }

    n->maxend = end;
    n->maxgap = gap;
  }

  template<class ...Args>
  void update(Args... args) {
    (update_impl(args), ...);
  }

  // Now `x` is full, so it has M - 1 keys.
  // Moves half of `x`'s keys into parent `p`.
  void split(node *p, int i, node *x) {
    constexpr int t = Order / 2;
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

    update(x, z, p);
  }

  void insert_impl(node *x, K key, const V &value) {
    int i = x->count - 1;

    if (x->leaf) {
      // Do a linear scan of the keys to find the insert position.
      // No need to binary search, we must move them anyway.
      while (i >= 0 && key < x->k[i]) {
        x->k[i + 1] = x->k[i];
        x->v[i + 1] = os::move(x->v[i]);
        i--;
      }
      x->k[i + 1] = key;
      x->v[i + 1] = value;
      // We've guaranteed this won't overflow, since when we descend,
      // we'll split children as needed.
      x->count++;
    } else {
      // Perhaps we can binary search this one?
      while (i >= 0 && key < x->k[i])
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
    update(x);
  }

  va_t find_gap_impl(node *n, size_t len) const {
    if (n->maxgap < len)
      return 0;

    for (int i = 0; i < n->count; i++) {
      if (!n->leaf && n->ch[i].maxgap >= len)
        return find_gap_impl(n->ch[i], len);

      if (n->leaf && i > 0) {
        size_t gap = n->k[i] - n->v[i - 1].end;
        if (gap >= len)
          return n->v[i - 1].end;
      }
    }
    // Don't forget the final one.
    if (!n->leaf)
      return find_gap_impl(n->ch[n->count], len);
    
    return 0;
  }

  void find_overlap_impl(node *n, va_t start, va_t end, vector<vma_t*>& result) const {
    if (!n || n->maxend <= start)
      return;

    for (int i = 0; i < n->count; i++) {
      if (!n->leaf)
        find_overlap_impl(n->ch[i], start, end, result);
      
      // If this node's start is already bigger than `end`, then its left child
      // might be OK, but its right child will always be too big.
      if (n->k[i] >= end)
        return;

      if (n->k[i] < end && n->v[i].end > start)
        result.push_back(&n->v[i]);
    }

    // Check rightmost child.
    if (!n->leaf)
      find_overlap_impl(n->ch[n->count], start, end, result);
  }

  void merge(node *p, int i) {
    node *l = p->ch[i];
    node *r = p->ch[i + 1];
    constexpr int t = Order / 2;
    // We are merging `p->k[i]` and its two children.
    // The merge target is `l`.

    // Copy p's data into `l`.
    l->k[t - 1] = p->k[i];
    l->v[t - 1] = os::move(p->v[i]);

    // Copy r's data into `l`.
    for (int j = 0; j < r->count; ++j) {
      l->k[j + t] = r->k[j];
      l->v[j + t] = os::move(r->v[j]);
    }
    if (!l->leaf) {
      for (int j = 0; j <= r->count; ++j)
        l->ch[j + t] = r->ch[j];
    }
    l->count = 2 * t - 1;

    // Remove the key from parent.
    for (int j = i; j < p->count - 1; ++j) {
      p->k[j] = p->k[j + 1];
      p->v[j] = os::move(p->v[j + 1]);
    }
    for (int j = i + 1; j < p->count; ++j)
      p->ch[j] = p->ch[j + 1];
    p->count--;
    
    delete r;
    update(l);
    update(p);
}
public:
  constexpr static int order = Order;
  using key_type = K;
  using value_type = V;

  btree() {
    // Always insert two sentinels.
    insert(0, vma_t(0, 0, /*prot=*/0, /*flags=*/0));
    insert(user_va_max, vma_t(user_va_max, user_va_max, /*prot=*/0, /*flags=*/0));
  }

  ~btree() {
    // Delete the entire tree.
    
  }

  void insert(K key, const V &value) {
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
      
      int i = s->k[0] < key;
      insert_impl(s->ch[i], key, value);
      root = s;
      update(root);
      return;
    }
    
    insert_impl(root, key, value);
  }

  void erase(node *n, K key) {
    int i = 0;
    while (i < n->count && n->k[i] < key)
      i++;

    // The key is in this node.
    if (i < n->count && n->k[i] == key) {
      if (n->leaf) {
        for (auto j = i + 1; j < n->count - 1; j++)
          n->k[j] = os::move(n->k[j + 1]);
        n->count--;
        update(n);
        return;
      }
      // Case 1b: It's an internal node. Replace with pred/succ.
      remove_from_internal(n, i);
      return;
    }

    // CASE 2: The key is in a subtree
    if (n->leaf)
      return; // Key not found

    // Before descending, ensure ch[i] has at least `t` keys.
    if (n->ch[i]->count < Order / 2)
      fill(n, i); // This is the "bolstering" logic

    // After fill, the key might have moved to ch[i-1] or been merged
    // so we figure out which child to descend into again.
    remove(i > n->count ? n->ch[i - 1] : n->ch[i], key);
    
    update(n); // Bottom-up update as recursion unwinds
  }

  // Returns a starting address with a gap at least `len`.
  va_t find_gap(size_t len) const {
    return find_gap_impl(root, len);
  }

  vector<vma_t*> find_overlap(va_t start, va_t end) const {
    vector<vma_t*> result;
    find_overlap_impl(root, start, end, result);
    return result;
  }
};

// Map according to the current process's VMA.
// Terminates the process when the pointer is not in any VMA.
void map_current(void *va);
void map_current(void *va, pte_t *pte);

// Map a range. Only maps the addresses that are currently unmapped.
// If `write` is set to true, also maps COW pages in the range.
void map_current(void *from, void *to, bool write = false);

struct addrspace {
  vector<vma_t> vmas;

  void split_at(size_t i, va_t addr);
  result merge_at(size_t i);
  
  vma_t &operator[](size_t index) { return vmas[index]; }
  const vma_t &operator[](size_t index) const { return vmas[index]; }

  void push(const vma_t &vma);
  bool has(va_t addr) const;

  // Finds the insertion place of `addr`, i.e. the first vma that is
  // greater than `addr`.
  // If `addr` is already contained, return that index.
  size_t find(va_t addr) const;
  
  vma_t &at(va_t addr) { return vmas[find(addr)]; }
  const vma_t &at(va_t addr) const { return vmas[find(addr)]; }
  void clear() { vmas.clear(); }

  vma_t *begin() { return vmas.begin(); }
  vma_t *end() { return vmas.end(); }
  size_t size() const { return vmas.size(); }

  void resize(size_t sz) { vmas.resize(sz); }
};

}

#endif
