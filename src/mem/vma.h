#ifndef VMA_H
#define VMA_H

#include "ptable.h"
#include "../fs/vfs.h"
#include "../utils/log.h"

#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20

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
template<int Order> requires(Order % 2 == 0)
class btree {
  using K = va_t;
  using V = vma_t;

  int sz = 0;

public:
  struct node {
    // Remember that #children = #keys + 1. Here `count` is the number of keys.
    // The maximum number of keys is Order - 1, while the minimum number of keys is ceil(Order / 2) - 1.

    node *ch[Order];    // Children.
    K k[Order - 1];     // Keys.
    V v[Order - 1];     // Values.
    size_t minstart;    // Min starting point in children.
    size_t maxend;      // Max ending point in children.
    size_t maxgap;      // Max gap in children.
    int count = 0;
    bool leaf;

    node(bool leaf): leaf(leaf) {}
  } *root = nullptr;
  
  // The minimum number of keys is t - 1, and the minimum number of children is t.
  static constexpr int t = Order / 2;

private:
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

#ifndef NDEBUG
  // Checks that `n` never violates invariant.
  // Note that it is not true that after every operation the invariant is maintained;
  // Specially, when the tree depth increases, `n == root` is not true yet, while the new root has only 1 child.
  void check(node *n) {
    if (n != root && (n->count < t - 1 || n->count >= Order)) {
      dump();
      printk("node: count = %d\n", n->count);
      assert(false && "btree: invariant broken");
    }
  }
#endif

  // Updates the `max` values, whenever a node itself or its children changes.
  // We don't need to be recursive, as all children are already up to date.
  void update_impl(node *n) {
    if (!n)
      return;

    size_t end = 0, gap = 0;
    n->minstart = n->leaf ? n->k[0] : n->ch[0]->minstart;
    
    for (int i = 0; i < n->count; i++) {
      end = max(end, n->v[i].end);
      if (n->leaf && i > 0)
        gap = max(n->maxgap, n->k[i] - n->v[i - 1].end);

      if (!n->leaf) {
        end = max(end, n->ch[i]->maxend);
        gap = max(gap, n->ch[i]->maxgap);
        // We know `ch[i]` is left of `k[i]`, and therefore its end might not reach `k[i]`'s end.
        // There might be a gap, and we calculate its length.
        assert(n->k[i] >= n->ch[i]->maxend);
        gap = max(gap, n->k[i] - n->ch[i]->maxend);
        // Similarly, we calculate the length of the right node here.
        assert(n->ch[i + 1]->minstart >= n->v[i].end);
        gap = max(gap, n->ch[i + 1]->minstart - n->v[i].end);
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

  void update_path_impl(node *n, va_t key) {
    if (!n)
      return;

    // Check whether the key is present.
    int i = 0;
    for (; i < n->count && key >= n->k[i]; i++) {
      if (n->k[i] == key) {
        update_impl(n);
        return;
      }
    }

    // Descend into the child if not found in this node.
    update_path_impl(n->ch[i], key);
    update_impl(n);
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

    update(x, z, p);
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

      update(l, r, x);
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

      update(l, r, x);
      return;
    }
    
    // Neither of left and right sibling have enough keys.
    // In this case, merging them will never exceed the capacity (Order - 1).
    assert(x->count > 0);
    merge(x, i < x->count ? i : i - 1);
  }

  void insert_impl(node *x, K key, const V &value) {
    int i = x->count - 1;
    while (i >= 0 && key < x->k[i])
      i--;
    if (i >= 0 && x->k[i] == key) {
      x->v[i] = value;
      update(x);
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
      update(x);
      return;
    }
    
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
        update(x);
        return;
      }
    }
    insert_impl(x->ch[i], key, value);
    update(x);
  }

  va_t find_gap_impl(node *n, size_t len, size_t min = 0) const {
    if (n->maxgap < len || n->maxend < min)
      return 0;

    for (int i = 0; i < n->count; i++) {
      // Three cases, corresponding to the cases in update().
      if (!n->leaf) {
        if (n->ch[i]->maxgap >= len && n->ch[i]->maxend >= min) {
          if (auto va = find_gap_impl(n->ch[i], len, min))
            return va;
        }

        size_t start = max(min, n->k[i]);
        if (n->k[i] >= start + len)
          return start;

        start = max(n->v[i].end, min);
        if (n->ch[i+1]->minstart >= start + len)
          return start;
      }

      if (n->leaf && i > 0) {
        size_t start = max(n->v[i - 1].end, min);
        if (n->k[i] >= start + len)
          return start;
      }
    }
    // Don't forget the final one.
    if (!n->leaf && n->ch[n->count]->maxend >= min)
      return find_gap_impl(n->ch[n->count], len, min);

    return 0;
  }

  void find_overlap_impl(node *n, va_t start, va_t end, vector<vma_t*> &result) const {
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

  // Basically the same as above, but early-exits.
  bool has_overlap_impl(node *n, va_t start, va_t end) const {
    if (!n || n->maxend <= start)
      return false;

    for (int i = 0; i < n->count; i++) {
      if (!n->leaf && has_overlap_impl(n->ch[i], start, end))
        return true;

      if (n->k[i] >= end)
        return false;

      if (n->k[i] < end && n->v[i].end > start)
        return true;
    }

    if (!n->leaf)
      return has_overlap_impl(n->ch[n->count], start, end);
    return false;
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
    update(l, p);
  }

  va_t id(node *x) {
    return (va_t) x - 0xffff'ffff'c000'0000;
  }

  void dump_node(node *x) {
    printk("  n%p [label=\"", id(x));

    for (int i = 0; i < x->count; i++) {
      printk("<f%d> %p", i, x->k[i]);
      if (i + 1 < x->count)
        printk(" | ");
    }
    printk("\"];\n");

    if (!x->leaf) {
      for (int i = 0; i <= x->count; i++)
        dump_node(x->ch[i]);
    }
  }

  void dump_edge(node *x) {
    if (x->leaf)
      return;

    for (int i = 0; i <= x->count; i++) {
      printk("  n%p -> n%p;\n", id(x), id(x->ch[i]));
      dump_edge(x->ch[i]);
    }
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
        update(n);
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
      update(n);
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
    update(n);
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

  // Produces a copy of the tree.
  node *copy_impl(node *x) {
    if (!x)
      return nullptr;

    node *n = new node(x->leaf);
    n->count = x->count;
    n->minstart = x->minstart;
    n->maxend = x->maxend;
    n->maxgap = x->maxgap;
    for (int i = 0; i < x->count; i++) {
      n->k[i] = x->k[i];
      n->v[i] = x->v[i];
    }

    if (!n->leaf) {
      for (int i = 0; i <= x->count; i++)
        n->ch[i] = copy_impl(x->ch[i]);
    }

    return n;
  }

public:
  constexpr static int order = Order;
  using key_type = K;
  using value_type = V;

  // Disable copying. It needs walking the entire tree - and it is slow.
  // This has to be avoided to make `libcbench` run faster.
  btree(const btree &other) {
    root = copy_impl(other.root);
  }

  btree &operator=(const btree &other) {
    clear();
    root = copy_impl(other.root);
    return *this;
  }

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

  // This will always use `vma.begin` as key.
  void insert(const V &value) {
    sz++;
    K key = value.begin;

    if (!root) {
      root = new node(true);
      root->k[0] = key;
      root->v[0] = value;
      root->count = 1;
      root->minstart = value.begin;
      root->maxend = value.end;
      root->maxgap = 0;
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

  // Returns a starting address with a gap at least `len`.
  // The address will be >= len.
  va_t find_gap(size_t len, size_t min = 0) const {
    return find_gap_impl(root, len, min);
  }

  void update_path(va_t key) {
    update_path_impl(root, key);
  }

  vector<vma_t*> find_overlap(va_t start, va_t end) const {
    vector<vma_t*> result;
    find_overlap_impl(root, start, end, result);
    return result;
  }

  bool has_overlap(va_t start, va_t end) const {
    return has_overlap_impl(root, start, end);
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

#ifndef NDEBUG
  // Dump a .dot file for visualization.
  void dump() {
    printk("digraph btree { \n  node [shape=record]\n");
    dump_node(root);
    dump_edge(root);
    printk("}\n");
  }
#endif
};

// Map according to the current process's VMA.
// Terminates the process when the pointer is not in any VMA.
void map_current(void *va);
void map_current(void *va, pte_t *pte);

// Map a range. Only maps the addresses that are currently unmapped.
// If `write` is set to true, also maps COW pages in the range.
void map_current(void *from, void *to, bool write = false);

// Initialize VMA.
void init();

struct addrspace {
  using map = btree<4>;
  using node = map::node;

  map vmas;
  va_t heap_begin, heap_end;
  va_t mmap_begin = 0x6000'0000;
  mutable vma_t *cache;

  addrspace() = default;
  ~addrspace() { vmas.clear(); }

  void split(va_t addr);
  // Unlike btree::find, this finds the VMA *containing* the address, rather than starting at the address.
  vma_t *find(va_t addr) const;
  va_t brk(va_t addr);
  va_t find_mmap(unsigned long len, va_t hint) const;

  void insert(const vma_t &vma);
  void erase(va_t begin) { vmas.erase(begin); }
  void clear() { vmas.clear(); }

  auto find_overlap(va_t begin, va_t end) const {
    return vmas.find_overlap(begin, end);
  }
};

}

#endif
