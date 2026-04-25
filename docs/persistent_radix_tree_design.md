# Persistent Radix Tree (Byte-Array Keys)

This document describes the design of the persistent radix tree used as the in-memory key directory in ByteCaskDB. It is intended for contributors who need to understand, modify, or reason about correctness of this component.

The **Background** section builds up the necessary concepts from scratch — persistent data structures, structural sharing, Tries, and Patricia Tries — for readers coming without that context. From §1 onward the document covers the C++ design: the overview, design principles, node layout, API, algorithms, acceptance criteria, memory usage, and benchmark results.

---

## Background

### What is a Persistent Data Structure?

A **persistent data structure** preserves all previous versions of itself when modified. Instead of mutating state in place, every operation returns a new version. Old versions are never altered and remain fully accessible.

> **Note on terminology**: "Persistent" here comes from functional programming — it refers to *immutability and version preservation*, not to storage on disk. A persistent data structure lives entirely in memory.

The simplest way to implement persistence is to deep-copy the entire structure on every write. That is O(N) per operation — correct, but impractical at any real scale.

The efficient alternative is **structural sharing**: since nodes are never mutated after creation, unchanged nodes can be *referenced by both the old and the new version simultaneously*. No copying is needed for any part of the structure that wasn't on the modified path.

For trees, a single insertion or deletion only touches nodes along the *path from the root to the affected leaf*. Everything off that path is shared freely between the two versions — this technique is called **[path copying](https://doi.org/10.1016/0022-0000(89)90034-2)**. Okasaki's [*Purely Functional Data Structures*](https://www.cambridge.org/9780521663502) (1998) develops this and related techniques in depth.

### Persistent BST: Path Copying

Consider a binary search tree holding integer keys. Nodes are identified by a pointer ID (e.g. `ptr_1`) and carry a key (e.g. `8`).

**Version 1** — `root_v1` holds a reference to `ptr_1`:

```mermaid
graph TB
    ptr_1["ptr_1 : 8"] --> ptr_2["ptr_2 : 4"]
    ptr_1 --> ptr_5["ptr_5 : 12"]
    ptr_2 --> ptr_3["ptr_3 : 2"]
    ptr_2 --> ptr_4["ptr_4 : 6"]
    ptr_5 --> ptr_6["ptr_6 : 10"]
    ptr_5 --> ptr_9["ptr_9 : 14"]
    ptr_6 --> ptr_7["ptr_7 : 9"]
    ptr_6 --> ptr_8["ptr_8 : 11"]
    ptr_9 --> ptr_10["ptr_10 : 13"]

    classDef sharedNode fill:#ADD8E6,stroke:#2166ac
    class ptr_1,ptr_2,ptr_3,ptr_4,ptr_5,ptr_6,ptr_7,ptr_8,ptr_9,ptr_10 sharedNode
```

The search path for inserting 5 is **ptr_1(8) → ptr_2(4) → ptr_4(6)**, where 5 is placed as the left child of `ptr_4` (since 5 < 6). Every node on this path must be copied because their child pointers change. Everything off the path is untouched.

The result is three copies (`ptr_11`, `ptr_12`, `ptr_13`) plus a new leaf (`ptr_14 : 5`). `root_v2` points to `ptr_11`. The remaining 7 nodes on the right subtree and the untouched left leaf are shared unchanged.

**Version 2** — `root_v2 → ptr_11` (after inserting key 5):

```mermaid
graph TB
    ptr_11["ptr_11 : 8"] --> ptr_12["ptr_12 : 4"]
    ptr_11 --> ptr_5["ptr_5 : 12"]
    ptr_12 --> ptr_3["ptr_3 : 2"]
    ptr_12 --> ptr_13["ptr_13 : 6"]
    ptr_13 --> ptr_14["ptr_14 : 5"]
    ptr_5 --> ptr_6["ptr_6 : 10"]
    ptr_5 --> ptr_9["ptr_9 : 14"]
    ptr_6 --> ptr_7["ptr_7 : 9"]
    ptr_6 --> ptr_8["ptr_8 : 11"]
    ptr_9 --> ptr_10["ptr_10 : 13"]

    classDef newNode fill:#90EE90,stroke:#2d7a2d
    classDef sharedNode fill:#ADD8E6,stroke:#2166ac
    class ptr_11,ptr_12,ptr_13,ptr_14 newNode
    class ptr_3,ptr_5,ptr_6,ptr_7,ptr_8,ptr_9,ptr_10 sharedNode
```

- **Green nodes** — newly allocated copies (`ptr_11`, `ptr_12`, `ptr_13`) plus the new leaf (`ptr_14 : 5`). Only 4 nodes out of 11 are new.
- **Blue nodes** — the exact same node objects from Version 1, shared by pointer. No copying occurred.

A caller holding `root_v1` sees the original tree, unchanged. A caller holding `root_v2` sees a tree that contains key 5. Both are valid simultaneously and neither is aware of the other.

Persistence adds O(log N) allocations per write — the length of the copied path — but **does not change the time complexity of any operation**.

### Tries: Branching on Key Bytes

A BST branches based on a *comparison* between whole keys. The path length depends on the number of keys in the tree (O(log N) for a balanced tree).

A **Trie** (from the word *re**trie**val*) takes a fundamentally different approach: it branches on *individual characters (or bytes)* of the key, one per level. The depth of any key equals its length, regardless of how many keys are in the tree.

- Each **edge** is labelled with a single character.
- A key's value is stored at the node reached after consuming all its characters.
- All keys sharing a common prefix share the same path down to the point of divergence — prefix sharing is structural, not incidental.

Keys: `app`, `apple`, `apply`, `apt`

```
root
 └─'a'─ node
          └─'p'─ node
                   ├─'p'─ [app ✓]
                   │        └─'l'─ node
                   │                ├─'e'─ [apple ✓]
                   │                └─'y'─ [apply ✓]
                   └─'t'─ [apt ✓]
```

Nodes marked ✓ carry a value. Unmarked nodes are routing-only intermediates.

Path copying applies exactly as in the BST. The path to any key has at most k nodes — one per character — so inserting or updating a key of length k copies at most k nodes. The rest of the trie is shared. Unlike the BST, path length is O(k) — bounded by the key length, not the number of keys N. For large N this is a significant advantage: lookup and write cost stay constant as the dataset grows.

### Patricia Tries (Radix Trees): Compressing the Trie

A standard Trie can contain long chains of single-child nodes — one per character of a shared prefix — that carry no branching information. A key `"application"` with no sibling sharing its prefix creates a linear chain 11 nodes deep. These nodes are pure structural overhead.

A **[Patricia Trie](https://dl.acm.org/doi/abs/10.1145/321479.321481)** (also called a *Radix Tree*) eliminates this by collapsing chains of single-child nodes into a single edge whose label carries the entire compressed byte sequence. Branching still happens at the first character where two keys diverge; it just doesn't allocate a separate node for every character in between.

**Standard Trie** — the `a → p` prefix creates a 2-node chain before the first branch:

```
root
 └─'a'─ node
          └─'p'─ node
                   ├─'p'─ [app ✓]
                   │        └─'l'─ node
                   │                ├─'e'─ [apple ✓]
                   │                └─'y'─ [apply ✓]
                   └─'t'─ [apt ✓]
```

**Patricia Trie** — the single-child chain `a → p` is collapsed into the edge label `"ap"`:

```
root
 └─"ap"─ node
            ├─"p"─ [app ✓]
            │        └─"l"─ node
            │                ├─"e"─ [apple ✓]
            │                └─"y"─ [apply ✓]
            └─"t"─ [apt ✓]
```

Node count drops from 7 to 5. For keys with long shared prefixes — say, UUID-keyed namespaces where every key starts with `"user::"` — the savings compound dramatically. A properly compressed Patricia Trie has at most 2N − 1 nodes for N keys — no unbounded single-child chains exist.

Path copying still applies. A key of length k touches at most k nodes on the path from root to leaf, so at most k nodes are copied per write. The edge-label compression reduces the *number* of nodes on that path in practice, meaning fewer allocations per write — but the bound remains O(k).

> **Naming**: Patricia Trie and Radix Tree refer to the same data structure. The rest of this document uses **Radix Tree**.

### Big-O Summary

Let N = total number of keys, k = length of the key being operated on.

| Structure | `get` | `set` | `erase` | New allocs / write |
|---|---|---|---|---|
| Mutable BST | O(log N) | O(log N) | O(log N) | O(1) |
| **Persistent BST** | O(log N) | O(log N) | O(log N) | **O(log N)** |
| Mutable Trie | O(k) | O(k) | O(k) | O(k) |
| **Persistent Trie** | O(k) | O(k) | O(k) | **O(k)** |
| Mutable Radix Tree | O(k) | O(k) | O(k) | O(k) |
| **Persistent Radix Tree** | O(k) | O(k) | O(k) | **O(k)** |

**Making a structure persistent does not change the asymptotic time complexity of any operation.**

---

## 1. Overview

This component implements a persistent radix tree in C++ as the key directory for ByteCaskDB. It exposes two interfaces: a fully immutable `PersistentRadixTree<V>` where every mutating operation returns a new version sharing unchanged subtrees by pointer, and a `TransientRadixTree<V>` builder for efficient bulk loading that converts to a persistent snapshot when done. Both support `get`, `set`, `erase`, ordered iteration, and `lower_bound`. A static `merge` operation combines two persistent trees in O(overlap) time, adopting disjoint subtrees by pointer without cloning. The module is `bytecask.radix_tree` and uses standard allocators.



## Design Principles

This component inherits the ByteCaskDB design tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Predictable latency over peak throughput**: Write-path operations must have bounded, predictable latency. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes.
4. **Performance**: Optimizations require a real use case. Without one, correctness and simplicity take priority.

**Key context**: this tree is an in-memory index in front of disk I/O that is orders of magnitude slower. CPU-bound optimizations matter only where they affect the write path's latency distribution (principle #3) or materially reduce memory overhead for large key sets. Complexity that doesn't serve one of these goals is cut.

## 2. Core Characteristics

*   **Key Type:** `std::span<const std::byte>` (Ingested and prefix-compressed natively).
*   **Value Type:** Generic `V`.
*   **Immutability:** All mutating operations return a new version of the tree. Untouched nodes are shared via intrusive reference-counted pointers (`IntrusivePtr<Node>`).
*   **Memory Management:** Standard allocators. Nodes embed their own reference count (`std::atomic<std::uint32_t>`) and are managed via `IntrusivePtr<T>`, a lightweight single-pointer (8 B) smart pointer that replaces `std::shared_ptr` (16 B + 32 B control block). This eliminates 32 bytes of `make_shared` control-block overhead per node and halves the pointer size in every child slot.
*   **Prefix Compression:** Shared byte sequences are stored once in the highest common parent node.
*   **Edit Tags (COW):** Transient mode uses epoch tags to safely mutate uniquely-owned nodes in-place, falling back to Path Copying when sharing occurs.

---

## 3. Architectural Design

### 3.1. Node Layout
```cpp
// Base node — 94% of all nodes are leaves and use only this struct.
struct Node {
    mutable std::atomic<uint32_t> refcount; // intrusive reference count
    uint32_t packed_tag; // high bit = has_value, bit 30 = has_children, low 30 bits = edit tag

    V value;

    // Short-prefix optimization: most prefixes after a split are 1–7 bytes.
    // Inline storage up to 7 bytes avoids a heap allocation per node.
    // Key segments longer than 7 bytes are split into a chain of routing
    // nodes (7 prefix bytes + 1 transition byte per hop).
    CompactPrefix prefix;  // 8 bytes (1-byte size + 7 inline bytes)
};

// Derived node for the ~6% of nodes that have children.
struct InternalNode : Node {
    // Struct-of-arrays layout: parallel vectors of transition bytes and
    // node pointers. Eliminates the 7 bytes of alignment padding that
    // pair<byte, IntrusivePtr> would waste per child slot (16 → 9 B/slot).
    struct ChildStore {
        std::vector<std::byte> transition_bytes;
        std::vector<IntrusivePtr<Node>> ptrs;
    };
    std::unique_ptr<ChildStore> children_;
};
```

`IntrusivePtr<T>` is a lightweight single-pointer (8 bytes) smart pointer. It calls `addref()` on copy and `release()` on destruction; when the count reaches zero, the node is deleted. Copy assignment uses addref-before-release sequencing to prevent use-after-free when the source is a sub-object of the destination (e.g., reassigning a root to one of its own children). Move assignment detaches the source pointer before releasing the old destination for the same reason. This eliminates the ~32-byte `make_shared` control block per node and halves the pointer size in every child slot from 16 bytes (`shared_ptr`) to 8 bytes (`IntrusivePtr`).

`Node::release()` uses an **iterative tail-release** to avoid the O(depth) recursive destructor chain that would otherwise result from `~IntrusivePtr → release → delete → ~Node → ~ChildStore → ~IntrusivePtr → …`. When the last reference to a node is dropped, the implementation detaches all children from their `IntrusivePtr`s before calling `delete`, then loops over those children releasing each one in turn — converting the last-child release into a loop. For chains of single-child nodes (the dominant pattern in prefix-compressed trees), this eliminates the recursive call overhead entirely. Profiling showed the naive recursive approach consuming 29% of total merge time at 100k keys.

`PersistentRadixTree` and `TransientRadixTree` use explicit move constructors/assignments that reset the source's `size_` (and `tag_` for transient) to zero via `std::exchange`. This ensures a moved-from tree is in a valid empty state (`size() == 0`, `empty() == true`) rather than carrying stale metadata while the root pointer has been transferred.

Leaf nodes use `Node` directly (no `children_` field — accessing it through a `Node*` is a compile error). Internal nodes use `InternalNode`, which adds a `unique_ptr<ChildStore>`. Child accessors on `Node` check a tag bit (`kHasChildrenBit`) before downcasting and return safe defaults ("no children") when the flag is absent. This makes incorrect access a compile-time error for leaves and a safe no-op for base-pointer dispatch.

Children use a struct-of-arrays layout (`ChildStore`): parallel vectors of transition bytes and node pointers. This eliminates the 7 bytes of alignment padding per child slot that `pair<byte, IntrusivePtr>` would waste (16 → 9 bytes per slot).

`CompactPrefix` is a fixed 8-byte container (1-byte size + 7 inline bytes, `alignof == 1`). Prefixes up to 7 bytes are stored inline with no heap allocation. Key segments longer than 7 bytes are split into a chain of routing nodes: each hop stores 7 prefix bytes plus 1 transition byte to the next node. This eliminates the heap spill path entirely — no heap allocation is ever needed for prefixes.

### 3.2. Transient Copy-on-Write (COW) Model
The `transient()` / `persistent()` API pattern — a mutable builder that freezes into an immutable snapshot — was popularised by [Rich Hickey's Clojure transients](https://clojure.org/reference/transients) (Clojure 1.1, ~2009).
*   A global `std::atomic<uint64_t>` generates unique edit tags for Transient sessions.
*   During a transient mutation, if the traversed node's `edit_tag` matches the session's tag **and** the node's reference count is 1 (uniquely owned), the node is mutated **in-place**.
*   If the tag differs (e.g., `0` or an older session) or the node is shared (refcount > 1), the node is **copied**, the copy is tagged with the current session ID, and the mutation applies to the copy.
*   The refcount guard defends against edit-tag wraparound: after 2^31 `transient()` calls the 31-bit tag space can repeat, but a shared node will never be mutated in place regardless of its tag.
*   A transient is single-use. `persistent() &&` retires the session tag, and any later operation on a consumed or moved-from builder throws `std::logic_error` in release builds instead of depending on debug-only assertions.

---

## 4. API Specification

### 4.1. Persistent API (`PersistentRadixTree<V>`)
All operations leave the original tree unchanged and return a new instance.

*   `PersistentRadixTree() = default`
*   `std::size_t size() const noexcept`
*   `bool empty() const noexcept`
*   `std::optional<V> get(std::span<const std::byte> key) const`
*   `bool contains(std::span<const std::byte> key) const`
*   `PersistentRadixTree set(std::span<const std::byte> key, V val) const` (Insert or overwrite).
*   `PersistentRadixTree erase(std::span<const std::byte> key) const`
*   `TransientRadixTree<V> transient() const` (Spawns a mutable builder).
*   `PersistentRadixTree merge(a, b, resolve)` (Static. Merges two trees; see §5.3).


### 4.2. Transient API (`TransientRadixTree<V>`)
Operations mutate the tree in-place utilizing the COW epoch logic. A consumed or moved-from transient is invalid for further use; every public entrypoint fails fast with `std::logic_error`.

*   `std::optional<V> get(std::span<const std::byte> key) const` *(Reads the current state of the transient tree)*
*   `bool contains(std::span<const std::byte> key) const`
*   `void set(std::span<const std::byte> key, V val)`
*   `template <typename Pred> std::optional<V> upsert(std::span<const std::byte> key, V val, Pred&& should_replace)` — Single-traversal insert-or-conditional-replace. If the key is absent, inserts `val` and returns `nullopt`. If the key is present, calls `should_replace(existing, val)`; if true, replaces the value and returns the displaced old value; if false, leaves the value unchanged and returns `nullopt`.
*   `bool erase(std::span<const std::byte> key)`
*   `PersistentRadixTree<V> persistent() &&` *(Consumes the builder; subsequent use throws `std::logic_error`)*


### 4.3. Iterator API (`Iterator`)
Because keys are fragmented across nodes, the iterator must materialize the key dynamically during Depth-First Search (DFS) traversal.

*   Satisfies `std::input_iterator` (single-pass). The iterator category is `std::input_iterator_tag` because `operator*` returns a prvalue pair containing a `span` into a mutable internal key buffer, not a true reference — this precludes `forward_iterator`.
*   Maintains a DFS stack and a `std::vector<std::byte> current_key`.
*   `operator*` returns `std::pair<std::span<const std::byte>, const V&>` — the materialized key and current value, suitable for structured bindings (`auto [k, v] = *it;`).
*   Supports `operator++` (pre-increment) to advance DFS.
*   Compares equal to `std::default_sentinel` when exhausted.
*   Required methods on Persistent tree: `begin()`, `end()`, `lower_bound(std::span<const std::byte> key)`.

---

## 5. Algorithmic Requirements

### 5.1. Insertion / Splitting

When inserting a key that diverges from an existing node's prefix (e.g., Node has "ABC", inserting "AXY"):
1.  Split the node into a Parent ("A") and a Child ("C", old value/children).
2.  Create a new Leaf ("Y", new value).
3.  Parent gets transition bytes 'B' (pointing to Child) and 'X' (pointing to Leaf), sorted by byte value.

```
  Before:                       After inserting "AXY":

  root                          root
   └─"ABC"─ [V1, ...]            └─"A"─ node
                                          ├─'B'─ "C"─ [V1, ...]
                                          └─'X'─ "Y"─ [V2]
```

### 5.2. Erasure / Path Compression

When removing a value (`node->value = std::nullopt`), the tree must maintain Patricia Trie invariants:
1.  **0 Children:** The node is deleted. Recursively check parent.
2.  **1 Child:** The node is merged with its child. The new prefix is `parent_prefix + transition_byte + child_prefix`.
3.  **>1 Child:** The node remains as a routing node.

```
  Notation: [✓] = node holds a value.  Nodes without [✓] are routing-only.

  Case 1 — 0 children (delete and propagate up):

  Before: erase "AB"             After:
  root                           root (empty)
   └─"A"─ node
             └─'B'─ "" [✓]


  Case 2 — 1 child (merge with child):

  Before: erase "A"              After:
  root                           root
   └─"A"─ [✓]                     └─"ABC"─ [✓]
             └─'B'─ "C"─ [✓]


  Case 3 — >1 children (keep as routing node):

  Before: erase "A"              After:
  root                           root
   └─"A"─ [✓]                     └─"A"─ (routing)
             ├─'B'─ "C"─ [✓]               ├─'B'─ "C"─ [✓]
             └─'X'─ "Y"─ [✓]               └─'X'─ "Y"─ [✓]
```

### 5.3. Merge

`merge(a, b, resolve)` combines two persistent trees into one, producing a new tree that shares unmodified subtrees from both inputs via `IntrusivePtr` copy (no node cloning).

**Signature:**
```cpp
template <typename ResolveFunc>
static auto merge(const PersistentRadixTree& a,
                  const PersistentRadixTree& b,
                  ResolveFunc&& resolve) -> PersistentRadixTree;
// resolve(const V& a_val, const V& b_val) -> V
```

**Algorithm — `merge_impl(node_a, node_b, resolve)`:** (The resolve callable is passed by lvalue in recursive calls to avoid `std::forward`-after-move UB with stateful or move-only resolvers.)

The function walks both trees in tandem, recursing only where the two trees overlap. At each step it compares the compressed prefixes of the two nodes. Let `cpl` = the common prefix length between `node_a.prefix` and `node_b.prefix`.

```
  Tree A:                 Tree B:                 Merged:

  root_a                  root_b                  root
   └─"ap"─ node            └─"ap"─ node            └─"ap"─ node
             ├─"p"─ [V1]             ├─"p"─ [V2]             ├─"p"─ [resolve(V1,V2)]
             └─"t"─ [V3]             └─"ple"─ [V4]           ├─"t"─ [V3]
                                                             └─"ple"─ [V4]

  "app"  conflict      → resolve(V1,V2) called.
  "apt"  only in A     → adopted by pointer, no clone.
  "apple" only in B    → adopted by pointer, no clone.
```
**Case 1 — Base cases:**
- If `node_a` is null, return `node_b`.
- If `node_b` is null, return `node_a`.
- In both cases the entire subtree is shared by pointer — O(1), no cloning.

**Case 2 — Prefixes diverge (`cpl < |prefix_a|` and `cpl < |prefix_b|`):**
- The two nodes live in disjoint parts of the key space.
- Create a new split node whose prefix is the common part (`prefix[0..cpl)`).
- Trim `node_a`'s prefix to `prefix_a[cpl+1..]`, insert it as a child under transition byte `prefix_a[cpl]`.
- Trim `node_b`'s prefix to `prefix_b[cpl+1..]`, insert it as a child under transition byte `prefix_b[cpl]`.
- Both subtrees (including all their descendants) are shared by pointer.

```
  Before:              After merge:
  A: "abcX..."          split: "abc"
  B: "abcY..."             ├─ 'X' → A (trimmed)
                           └─ 'Y' → B (trimmed)
```

**Case 3 — `node_b`'s prefix is exhausted first (`cpl < |prefix_a|`, `cpl == |prefix_b|`):**
- `node_b` sits *above* `node_a` in the trie (b is a prefix of a).
- Clone `node_b`, trim `node_a`'s prefix, and insert/merge `node_a` as a child of the clone at transition byte `prefix_a[cpl]`.

```
  A: "abc"─ [V1]        B: "ab"─ [V2]         Merged: "ab"─ [V2] (clone of B)
                                                          └─'c'─ ""-─ [V1]  (A trimmed)
```

**Case 4 — `node_a`'s prefix is exhausted first (`cpl == |prefix_a|`, `cpl < |prefix_b|`):**
- Symmetric to Case 3. Clone `node_a`, trim `node_b`, insert/merge as child.

**Case 5 — Full prefix match (`cpl == |prefix_a| == |prefix_b|`):**
- The two nodes correspond to the same trie position.
- Clone `node_a`. If both have a value, call `resolve(a.value, b.value)` to pick the winner. If only `b` has a value, copy it.
- Walk `node_b`'s child list and for each `(transition_byte, child_b)`:
  - If `node_a` has a child with the same transition byte, recurse: `merge_impl(child_a, child_b, resolve)`.
  - Otherwise, adopt `child_b` directly (O(1) `IntrusivePtr` copy — the entire disjoint subtree is shared).
- *This is the case shown in the overview diagram above:* both roots share the prefix `"ap"`, so they are cloned and their children merged recursively.

**Size computation:**

The merged tree's `size_` is computed as `a.size() + b.size() - overlaps`, where `overlaps` is the number of keys present in both trees (i.e. where `resolve` was called). `merge_impl` tracks this count during traversal and returns it alongside the merged root, avoiding an O(N) post-merge walk.

**Complexity:**

| Scenario | Cost |
|---|---|
| Fully disjoint trees (no shared keys) | O(1) per subtree adoption |
| Fully overlapping trees (all keys shared) | O(N) — must visit every conflicting leaf |
| Partial overlap | O(overlap) for merge |

The structural sharing guarantee: any subtree that exists in only one input is adopted by pointer without cloning any of its nodes. Cloning only occurs on the path from the root to each conflict point.

**Conflict resolution:**

The `resolve` callback is only invoked on exact key conflicts (same key present in both trees). It receives the two values by `const&` and returns the winner. Common resolvers:
- Recovery (higher LSN wins): `[](auto& a, auto& b) { return b.lsn > a.lsn ? b : a; }`
- Prefer b: `[](auto&, auto& b) { return b; }`

**Use case — parallel recovery:**

Hint files are assigned to workers round-robin. Each worker builds a `TransientRadixTree`, converts it to persistent, and the results are merged pair-wise in a fan-in tree (log₂(N) rounds). The merge handles overlapping keys (same key updated across multiple data files) via the LSN resolver.

---

## 6. Acceptance Criteria
1.  **Immutability:** Verifiable tests showing that `t2 = t1.set(...)` does not alter `t1`'s observable state or iterators.
2.  **Prefix Reuse:** Memory profiling or node-inspection tests proving that inserting "prefix_A" and "prefix_B" creates exactly one shared node for "prefix_".
3.  **Transient Efficiency:** Google Benchmark suite (`benchmarks/map_bench.cpp`, target `bytecask_bench`) comparing `PersistentRadixTree` against `PersistentOrderedMap` across persistent set, transient set, get, iteration, and lower_bound at 1k/10k/100k keys. Transient bulk insert must be significantly faster than chained persistent `set()` for the same container.
4.  **Ordered Iteration:** `std::is_sorted` returns true when iterating from `begin()` to `end()` comparing `iterator.key()`.
5.  **CompactPrefix inline storage:** Inserting keys that produce splits with prefixes ≤ 15 bytes does not trigger heap allocations for prefix storage (verified via allocator instrumentation or node inspection).
6.  **Erase Compaction:** Erasing all keys from a tree results in a completely empty root, with no dangling routing nodes.
7.  **Model-based property tests:** A deterministic PRNG-driven test generates 10,000 random `set`/`erase` operations over short byte-array keys (alphabet of 6, length 1–8 for high prefix overlap) and applies them to the radix tree and a `std::map<std::string, int>` oracle. After every operation the following invariants are checked:
    - `size()` equals `oracle.size()`.
    - `get(k)` returns the same value (or `nullopt`) as the oracle for the operated key.
    - `contains(k)` agrees with `oracle.count(k)` for the operated key.
    - Every 50 rounds: full iteration yields the same key-value sequence as iterating the oracle in ascending order.
    - Every 100 rounds: `lower_bound(probe)` returns the same first key/value as `oracle.lower_bound(probe)` for a random probe key.
    - Every 200 rounds: a snapshot taken before the mutation still matches the entries captured at that point (immutability).

    The fixed seed makes any failure deterministic and reproducible; the round number in `INFO` pinpoints the failing operation without shrinking.
8.  **Memory safety:** Full test suite (82 test cases, 1M+ assertions) passes under Clang AddressSanitizer + LeakSanitizer with zero errors. Build via `xmake f --sanitizer=address -m debug`.
9.  **Concurrent reader/writer safety:** A dedicated test (`[concurrency]` tag) spawns 4 reader threads iterating a persistent snapshot while a writer thread mutates a transient derived from the same snapshot. All readers observe a consistent, unchanged snapshot throughout. ThreadSanitizer verification requires ASLR control (`xmake f --sanitizer=thread -m debug`; run with `setarch -R` or lowered `vm.mmap_rnd_bits`).
10. **Memory footprint:** `BM_MemoryFootprint` benchmarks in `map_bench.cpp` report heap bytes allocated per key for each container at 1k/10k/100k keys, enabling relative comparison of memory overhead.

---

## 7. Memory Usage

### 7.1. Node layout breakdown

Nodes are allocated via `new` and managed by `IntrusivePtr<Node>`, which embeds the reference count inside the node itself. No separate control block is allocated. Leaf nodes (94% of all nodes) use the base `Node<V>` struct; the remaining ~6% use `InternalNode<V>` which adds a `children_` pointer.

**`Node<V>` (base — used for leaves):**

| Field | Type | Bytes |
|---|---|---|
| `refcount_` | `atomic<uint32_t>` | 4 |
| `packed_tag_` | `uint32_t` | 4 |
| `value_` | `V` (e.g. `KeyDirEntry`, 24 B) | 24 |
| `prefix` | `CompactPrefix` (7 inline bytes, alignof 1) | 8 |
| **Node struct total** | | **40 bytes** |

**`InternalNode<V>` (derived — used for routing nodes with children):**

| Field | Type | Bytes |
|---|---|---|
| (inherits `Node<V>`) | | 40 |
| `children_` | `unique_ptr<ChildStore>` | 8 |
| **InternalNode struct total** | | **48 bytes** |

Internal nodes heap-allocate a `ChildStore` (struct-of-arrays: two `std::vector`s, ~48 B header + N × 9 B per child slot). One child slot is 1 byte (transition) + 8 bytes (`IntrusivePtr`) = **9 bytes** — no alignment padding thanks to the struct-of-arrays split.

| Node type | Fraction | Struct | Heap children | Total |
|---|---|---|---|---|
| Leaf (0 children) | ~94% | 40 B | 0 | **40 B** |
| Internal (2 children avg) | ~6% | 48 B | ~66 B | ~114 B |
| **Weighted average** | | | | **~44 B** |

The ~44 B weighted average is the per-node cost. Measured per-key overhead is higher (~61–69 B/key) because prefix chain-splitting for long keys creates additional routing nodes, pushing the nodes-per-key ratio above 1.07.

### 7.2. Node distribution

For a typical key set with reasonable prefix compression the tree has approximately 1.07 nodes per key (a small constant overhead for routing nodes at split points). Leaf nodes (0 children) make up ~94% of nodes; internal routing nodes (~6%) spill their `children` vector to the heap because they fan out over many transition bytes (e.g. the 16 hex-digit branches of a UUID segment).

| Node type | Fraction | Children storage |
|---|---|---|
| Leaf (0 children) | ~94% | `nullptr` (0 B) |
| Internal (2+ children) | ~6% | heap `ChildStore` (~48 B header + N × 9 B) |

### 7.3. Measured footprint

Values from `BM_MemoryFootprint` at 100k keys with `KeyDirEntry` value type (measured with the global allocator tracker):

| Container | Key type | B/key (generic) | B/key (prefixed UUIDv7) | Key-length sensitivity |
|---|---|---|---|---|
| `PersistentRadixTree` | `span<byte>` (not stored) | **61 B** | **63 B** | Low — prefix compression absorbs shared bytes |
| `std::map` | `std::string` | 72 B | 117 B | High — full key copied into every node |

### 7.4. Prefix compression in practice

Prefix compression is not free: it replaces raw key bytes with tree structure. The trade-off is:

- **What gets compressed**: bytes shared between keys with a common prefix are stored once in a routing node rather than repeated in every leaf.
- **What gets paid**: each leaf node carries 40 bytes of struct overhead; internal nodes carry 48 B struct + a heap-allocated `ChildStore`. The weighted average is ~44 B/node, but prefix chain-splitting for long keys adds routing nodes that push the effective per-key cost higher.

For short, dissimilar keys (e.g. the generic `"key_N"` benchmark, 6–10 bytes each), compression is minimal and the per-node overhead dominates. For long, highly-redundant keys (e.g. `"user::018f6e2c-XXXX-7000-8000-XXXXXXXXXXXX"`, 45 bytes, 5 prefixes × 20k keys), RadixTree absorbs the extra key length at a modest per-key cost while `std::map` pays the full key storage, confirming that prefix compression is working correctly.

### 7.5. 100M key projections

The primary concern for ByteCaskDB is the key directory at production scale. The table below uses the measured ~69 B/key at 1M text keys as the per-key baseline.

| Configuration | B/key | 100M keys |
|---|---|---|
| Current (`Node`/`InternalNode` split, struct-of-arrays children, `CompactPrefix` 8 B, packed tag) | **~69** | **~6.9 GB** |
| `std::map` (no persistence, no prefix compression) | 72 | ~7.2 GB |

The original `shared_ptr`-based design measured 129 B/key (generic) and 139 B/key (prefixed). Intrusive refcounting reduced this to 108/116 B/key (−17%). The leaf node optimization (null `unique_ptr` instead of empty SmallVector) reduced to 86/92 B/key (−20% from intrusive, −33% from original). Replacing `SmallVector<byte,24>` (32 B) with `CompactPrefix` (16 B) and reordering `KeyDirEntry` fields to eliminate alignment padding further reduced to 70/74 B/key (−19% from leaf optimization, −46% from original). Shrinking `CompactPrefix` from 16 B to 8 B with chain-splitting for long prefixes, replacing child slot `pair<byte, IntrusivePtr>` with struct-of-arrays (`ChildStore`), and splitting `Node`/`InternalNode` so leaves allocate 40 B instead of 56 B brought the footprint to ~61/63 B/key at 100k and ~69 B/key at 1M (−47% from original).

### 7.6. Memory footprint by key shape

Measured at 1M keys using RSS-based profiling (`memory_profile.cpp`) with 245-byte values and `KeyDirEntry` (16 B). **B/key** is total DB RSS overhead divided by key count. **Overhead** subtracts the average key length — the structural cost the tree adds beyond the raw key bytes.

| Key shape | Avg key size | B/key | Overhead | Description |
|---|---|---|---|---|
| prefixed | 44 B | 50 | 6 | Type-prefixed UUIDv7 (`user::018f6e2c-...`). 5 prefix groups share a long common stem — best case for prefix compression. |
| uuidv7_binary | 16 B | 51 | 35 | Raw 16-byte UUIDv7. Binary encoding of time-ordered UUIDs — compact on disk and in the tree. |
| incremental | 4 B | 54 | 50 | Auto-increment integers (`"1"` .. `"1000000"`). Short keys with a growing common prefix per decimal digit. |
| uniform | 8 B | 54 | 46 | Sequential `"key_0"` .. `"key_N"`. Shared `"key_"` prefix, then digit fanout. |
| hash_prefixed | 24 B | 60 | 36 | Cassandra/DynamoDB pattern: 8-char hash partition + `::item::` + ordered sort key. 8 partitions, good compression within each. |
| uuidv7 | 36 B | 60 | 24 | RFC 9562 time-ordered UUIDs (text). 48-bit timestamp prefix shared across ~50 keys/ms — good compression. |
| zipfian | 27 B | 60 | 33 | Skewed hot/cold: 5% short hot keys, 95% longer cold keys with partition prefixes. |
| binary | 8 B | 65 | 57 | Non-ASCII keys with embedded `0x00`, `0x80`, `0xFF` bytes. Tests byte-level prefix comparison edge cases. |
| clustered | 16 B | 83 | 67 | Few large partitions (`users/`, `orders/`, `events/`) with 10-digit counters. Wide fanout at counter digit positions. |
| many_partitions | 13 B | 105 | 92 | 500k distinct 6-digit partition prefixes, ~2 keys each. Wide top-level fanout, minimal prefix sharing. |
| uuidv4_binary | 16 B | 181 | 165 | Raw 16-byte random UUIDv4. No prefix structure — every byte diverges early. |
| sha256_bin | 32 B | 409 | 377 | Raw 32-byte SHA-256 hashes. Fully random content, no shared prefix. Worst case for binary keys. |
| uuidv4_text | 36 B | 434 | 398 | Random UUIDv4 text (`550e8400-e29b-...`). Only the dashes at fixed positions are shared. |
| uuidv4_prefixed | 44 B | 464 | 420 | Type-prefixed random UUIDv4 (`user::550e8400-...`). Prefix saves ~6 B/key vs bare UUIDv4, but the random suffix dominates. |
| sha256_hex | 64 B | 881 | 817 | 64-char hex SHA-256 hashes. Fully random, 4 bits of entropy per character — worst case overall. |

Three tiers emerge:

1. **~50–65 B/key** — keys with strong prefix structure (time-ordered UUIDs, type prefixes, sequential integers, hash-partitioned sort keys). Structural overhead is 6–57 B above key size. This is the target range for most database workloads.
2. **~80–105 B/key** — keys with moderate structure but wide fanout (clustered counters, many small partitions). Still practical at scale.
3. **~180–880 B/key** — random keys (UUIDv4, SHA-256). The radix tree cannot compress what has no structure. Each key byte that diverges from its neighbours creates a separate routing node. At 1M SHA-256 hex keys the tree uses ~840 MB of RSS.

For production workloads, prefer time-ordered (UUIDv7) or prefix-structured keys. If keys are inherently random (content-addressed hashes), store the binary form (32-byte SHA-256 binary = 409 B/key) rather than hex (64-byte SHA-256 hex = 881 B/key) — the 2× key length reduction yields a 2× memory reduction.

---

## 8. Benchmark Results

Full benchmark suite run on 2 × 3.49 GHz vCPUs, Clang release build. Three containers compared:

- **RadixTree** — `PersistentRadixTree<int>` (this implementation)
- **OrderedMap** — `PersistentOrderedMap<Key, int>` (backed by `immer::flex_vector`)
- **StdMap** — `std::map<std::string, int>` (mutable baseline, no persistence)

### 8.1. Bulk insert (persistent `set()`, chained)

| Container | 1k keys (ns) | 10k keys (ns) | 100k keys (ns) |
|---|---|---|---|
| RadixTree | 758 k | 10.0 M | 123 M |
| OrderedMap | 2,493 k | 44.9 M | 385 M |
| StdMap | 203 k | 2.8 M | 43 M |

RadixTree persistent set is **~3× faster** than OrderedMap. StdMap (mutable, no COW) is the fastest baseline.

### 8.2. Bulk insert (transient `set()`)

| Container | 1k keys (ns) | 10k keys (ns) | 100k keys (ns) |
|---|---|---|---|
| RadixTree | 136 k | 1.9 M | 39 M |
| OrderedMap | 1,780 k | 28.9 M | 396 M |
| StdMap | 203 k | 2.8 M | 43 M |

RadixTree transient set is **~10× faster** than OrderedMap and comparable to StdMap at 100k keys.

### 8.3. Point lookup (`get()`)

| Container | 1k keys (ns) | 10k keys (ns) | 100k keys (ns) |
|---|---|---|---|
| RadixTree | 69 | 98 | 114 |
| OrderedMap | 292 | 488 | 661 |
| StdMap | 103 | 167 | 233 |

RadixTree get is **~6× faster** than OrderedMap and **~2× faster** than StdMap at 100k keys.

### 8.4. Iteration (full scan)

| Container | 1k keys (ns) | 10k keys (ns) |
|---|---|---|
| RadixTree | 28.8 k | 281 k |
| OrderedMap | 2.0 k | 28.2 k |
| StdMap | 6.7 k | 100 k |

RadixTree iteration is slower because the DFS iterator materializes keys by concatenating prefixes at each node — a cost inherent to key-compressed tries. OrderedMap iterates a flat chunk sequence and is fastest.

### 8.5. Lower bound

| Container | 1k keys (ns) | 10k keys (ns) | 100k keys (ns) |
|---|---|---|---|
| RadixTree | 317 | 380 | 462 |
| OrderedMap | 211 | 403 | 545 |
| StdMap | 92 | 151 | 224 |

RadixTree and OrderedMap converge at 100k keys. StdMap is ~2× faster (simpler tree structure, no key materialization).

### 8.6. Memory footprint (generic keys, `"key_N"`)

| Container | 1k B/key | 10k B/key | 100k B/key |
|---|---|---|---|
| RadixTree | 86 | 86 | **86** |
| OrderedMap\* | 7 | 8 | 40 |
| StdMap | 72 | 72 | **72** |

### 8.7. Memory footprint (prefixed UUIDv7 keys)

| Container | 1k B/key | 10k B/key | 100k B/key |
|---|---|---|---|
| RadixTree | 92 | 92 | **92** |
| OrderedMap\* | 44 | 44 | 75 |
| StdMap | 117 | 117 | **117** |

\* OrderedMap figures measure net allocations during incremental build; they undercount the steady-state RAM of a single live snapshot (see §7.6).

RadixTree is **21% cheaper** than StdMap for prefixed keys (92 vs 117 B/key) — this is where prefix compression pays off. For generic short keys it is 19% more expensive than StdMap (86 vs 72), which is the cost of prefix-compression bookkeeping on keys that share little structure.

### 8.8. Summary

| Operation | RadixTree vs OrderedMap | RadixTree vs StdMap |
|---|---|---|
| Persistent set | **3× faster** | 2.8× slower |
| Transient set | **10× faster** | ~1× (parity) |
| Get | **6× faster** | **2× faster** |
| Iteration | 2.8× slower | 2.8× slower |
| Lower bound | ~1× (parity) | 2× slower |
| Memory (generic) | see §7.6\* | +19% |
| Memory (prefixed) | see §7.6\* | **−21%** |

### 8.9. Merge

Merge-only cost (two pre-built N/2-key trees, merge step only):

| Scenario | 1k (µs) | 10k (µs) | 100k (µs) |
|---|---|---|---|
| Disjoint (0% overlap) | 23 | 261 | 2,779 |
| Overlapping (50% overlap) | 44 | 454 | 6,208 |
| **Ratio** | **1.9×** | **1.7×** | **2.2×** |

Overlapping merges are ~2× more expensive due to node cloning and conflict resolution at every shared key. Disjoint merges adopt entire subtrees via `IntrusivePtr` copy.

### 8.10. Split-build-merge vs linear build

End-to-end: `build(N/2) + build(N/2) + merge(N)` vs `build(N)`. All sequential.

| Benchmark | 1k (µs) | 10k (µs) | 100k (µs) | vs linear |
|---|---|---|---|---|
| TransientSet (linear baseline) | 91 | 1,056 | 14,583 | 1.0× |
| SplitBuildMerge (disjoint) | 117 | 1,395 | 19,820 | 1.36× |
| SplitBuildMergeOverlapping (20%) | 132 | 1,606 | 24,744 | 1.70× |
| SplitBuildMergePrefixed (disjoint) | 114 | 1,247 | 17,468 | 1.20× |

Sequential split+merge is 36–70% slower than linear depending on overlap. However, in a parallel execution model the two build phases overlap on separate threads, so the wall-clock cost becomes `max(build_a, build_b) + merge` rather than the sum.

Merge cost as a fraction of linear build:

| Key type | Merge / Linear | Merge (µs) |
|---|---|---|
| Generic, disjoint | 19% | 2,779 |
| Generic, 20% overlap | 34% | ~4,924 |
| Prefixed, disjoint | ~20% | ~2,885 |

Merge overhead is small enough that parallelising the build phase pays for itself with ≥2 threads.

---

## 9. Additional Considerations

### VM pressure and prefix-aligned hot/cold access

ByteCaskDB's stated operating envelope is all keys in physical RAM. However, the radix tree has a useful emergent property under memory pressure: because the tree's structure mirrors key prefixes, subtrees for different prefixes occupy disjoint pointer graphs and thus tend to land on disjoint OS pages. When access has clear prefix-aligned hot/cold locality (e.g., `user:<uuidv7>` where recent UUIDs are warm and old ones are cold), the OS VM subsystem acts as an implicit buffer pool — keeping warm subtrees resident and transparently paging out cold ones.

Under this pattern, latency becomes bimodal (fast for warm keys, a page-fault away for cold ones) but the system remains correct and the average case is dominated by the warm path. Full-tree scans defeat this property by pulling the entire working set back into RAM and should be avoided or rate-limited.

A manually managed buffer pool could replicate this behaviour, but Linux's VM is a decades-hardened implementation with adaptive LRU (active/inactive list split), readahead heuristics, NUMA awareness, and transparent huge pages. A userspace buffer pool would need to reimplement all of that to reach parity — and would almost certainly lose. Delegating to the OS is therefore not just simpler (principle #2) but likely better in practice.

---

## Appendix A: PersistentOrderedMap (retired predecessor)

`PersistentOrderedMap<K, V>` was the key-directory implementation that `PersistentRadixTree` replaced. The benchmark tables in §8 use **OrderedMap** as the persistent-data-structure comparison point; this appendix explains what that container was.

### What it is

A persistent, sorted associative container backed by [`immer::flex_vector<Entry>`](https://github.com/arximboldi/immer) — a Radix Balanced Tree (RBT) of 32-element chunks. Every mutating operation returns a new version that shares unmodified RBT chunks with the original. Iteration order is ascending by `K::operator<`.

```
Module:  bytecask.persistent_ordered_map
Key:     K  (std::totally_ordered + std::copyable)
Value:   V  (std::copyable)
Storage: immer::flex_vector<Entry>   — sorted, persistent RBT
```

### Core API

| Method | Description | Complexity |
|---|---|---|
| `get(key)` | Returns `std::optional<V>` | O(log n) |
| `contains(key)` | Predicate | O(log n) |
| `lower_bound(key)` | Iterator to first entry ≥ key | O(log n) |
| `set(key, val)` | Insert or overwrite; returns new version | O(log n) |
| `erase(key)` | Remove; returns new version | O(log n) |
| `transient()` | Spawns a mutable `OrderedMapTransient<K,V>` builder | O(1) |

### Transient builder (`OrderedMapTransient<K, V>`)

The transient builder wraps `immer::flex_vector_transient` and mutates RBT nodes in place until `persistent()` is called (freezes the tree in O(1)).

| Operation | Path | Complexity |
|---|---|---|
| `set` — overwrite existing key | mutates node in place | O(1) amortised |
| `set` — append at tail (sorted bulk load) | `push_back` only | **O(1) amortised** |
| `set` — insert in the middle | freeze → split → push → rejoin | O(log n) |
| `erase` | freeze → erase at index → refreeze | O(log n) |
| `persistent()` | freeze transient RBT | O(1) |

The O(1) tail-append path made pre-sorted bulk loading substantially faster than chained persistent `set()` calls, but it required the caller to insert keys in ascending order to exploit it.

### Why it was replaced

`PersistentOrderedMap` stores the **full key** in every `Entry`. It has no notion of shared prefixes: two keys `"user::018f6e2c-aaa"` and `"user::018f6e2c-bbb"` each carry the entire 25+ byte string. Memory scales linearly with key length × N.

`PersistentRadixTree` compresses shared prefixes into internal nodes. For the prefixed-UUIDv7 workload this reduces per-key memory from ~117 B (std::map baseline) to ~92 B (−21%). It also delivers ~6× faster point lookups and ~10× faster transient bulk insert at 100k keys, with the only cost being ~2.8× slower full iteration (DFS key materialisation overhead).

The `immer::flex_vector` chunk-sharing model also made accurate memory accounting difficult: the allocation tracker reported only *net* bytes for the final snapshot, undercounting the steady-state cost of a live map (see §7.6). `PersistentRadixTree` uses a straightforward `new`-per-node model that the allocation tracker counts accurately.

---

## Appendix B: References and Prior Art

Listed as a matter of good faith — this design builds on established ideas from the literature and from prior open-source work. If you know of a missing reference, please open a PR.

| Concept | Source | Link |
|---|---|---|
| Patricia Trie | Morrison, *J. ACM* 15(4), 1968 | https://dl.acm.org/doi/abs/10.1145/321479.321481 |
| Persistent data structures (path copying) | Driscoll, Sarnak, Sleator & Tarjan, *JCSS* 38(1), 1989 | https://doi.org/10.1016/0022-0000(89)90034-2 |
| Persistent data structures (accessible introduction) | Okasaki, *Purely Functional Data Structures*, Cambridge University Press, 1998 | [book](https://www.cambridge.org/9780521663502) · [OCaml source](https://github.com/mmottl/pure-fun) |
| Transient/persistent duality | Rich Hickey, Clojure (transients added in Clojure 1.1, ~2009) | https://clojure.org/reference/transients |
| Persistent C++ containers (benchmark baseline) | immer — Juan Pedro Bolívar Puente (arximboldi) | https://github.com/arximboldi/immer |
