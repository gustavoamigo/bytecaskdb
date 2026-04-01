# Design : Persistent Radix Tree (Byte-Array Keys)

## 1. Objective
Implement a Persistent (Immutable) Radix Tree (Patricia Trie) in C++ that provides native prefix-compression for byte-array keys, $O(1)$ snapshotting via structural sharing, ordered iteration (`lower_bound`), and efficient bulk updates via a Transient API. The container uses standard allocators.

## Design Principles

This component inherits the ByteCask design tenets in order of priority:

1. **Correctness**: Data integrity is paramount. All design decisions prioritize correctness over performance.
2. **Simplicity**: The architecture is kept simple to facilitate understanding and maintainability.
3. **Predictable latency over peak throughput**: Write-path operations must have bounded, predictable latency. A steady 1 ms per write is preferable to an average of 0.1 ms with occasional 500 ms spikes.
4. **Performance**: Optimizations are pursued only when they don't compromise correctness or simplicity.

**Key context**: this tree is an in-memory index in front of disk I/O that is orders of magnitude slower. CPU-bound optimizations matter only where they affect the write path's latency distribution (principle #3) or materially reduce memory overhead for large key sets. Complexity that doesn't serve one of these goals is cut.

### VM pressure and prefix-aligned hot/cold access

ByteCask's stated operating envelope is all keys in physical RAM. However, the radix tree has a useful emergent property under memory pressure: because the tree's structure mirrors key prefixes, subtrees for different prefixes occupy disjoint pointer graphs and thus tend to land on disjoint OS pages. When access has clear prefix-aligned hot/cold locality (e.g., `user:<uuidv7>` where recent UUIDs are warm and old ones are cold), the OS VM subsystem acts as an implicit buffer pool — keeping warm subtrees resident and transparently paging out cold ones.

Under this pattern, latency becomes bimodal (fast for warm keys, a page-fault away for cold ones) but the system remains correct and the average case is dominated by the warm path. Full-tree scans defeat this property by pulling the entire working set back into RAM and should be avoided or rate-limited.

A manually managed buffer pool could replicate this behaviour, but Linux's VM is a decades-hardened implementation with adaptive LRU (active/inactive list split), readahead heuristics, NUMA awareness, and transparent huge pages. A userspace buffer pool would need to reimplement all of that to reach parity — and would almost certainly lose. Delegating to the OS is therefore not just simpler (principle #2) but likely better in practice.

## 2. Core Characteristics
*   **Key Type:** `std::span<const std::byte>` (Ingested and prefix-compressed natively).
*   **Value Type:** Generic `V`.
*   **Immutability:** All mutating operations return a new version of the tree. Untouched nodes are shared via intrusive reference-counted pointers (`IntrusivePtr<Node>`).
*   **Memory Management:** Standard allocators. No `std::pmr`. Nodes embed their own reference count (`std::atomic<std::uint32_t>`) and are managed via `IntrusivePtr<T>`, a lightweight single-pointer (8 B) smart pointer that replaces `std::shared_ptr` (16 B + 32 B control block). This eliminates 32 bytes of `make_shared` control-block overhead per node and halves the pointer size in every child slot.
*   **Prefix Compression:** Shared byte sequences are stored once in the highest common parent node.
*   **Edit Tags (COW):** Transient mode uses epoch tags to safely mutate uniquely-owned nodes in-place, falling back to Path Copying when sharing occurs.

---

## 3. Architectural Design

### 3.1. Node Layout
```cpp
struct Node {
    mutable std::atomic<uint32_t> refcount; // intrusive reference count
    uint32_t packed_tag; // high bit = has_value, low 31 bits = edit tag (0 = immutable)

    V value;

    // Short-prefix optimization: most prefixes after a split are 1–20 bytes.
    // Inline storage up to 24 bytes avoids a heap allocation per node.
    SmallVector<std::byte, 24> prefix;

    // Heap-allocated children vector. nullptr for leaf nodes (94% of nodes).
    // Leaves pay only 8 bytes (the null pointer) instead of 32 bytes for
    // an empty SmallVector.
    using ChildVec = std::vector<std::pair<std::byte, IntrusivePtr<Node>>>;
    std::unique_ptr<ChildVec> children_;
};
```

`IntrusivePtr<T>` is a lightweight single-pointer (8 bytes) smart pointer. It calls `addref()` on copy and `release()` on destruction; when the count reaches zero, the node is deleted. This eliminates the ~32-byte `make_shared` control block per node and halves the pointer size in every child slot from 16 bytes (`shared_ptr`) to 8 bytes (`IntrusivePtr`).

Children are stored behind a `unique_ptr` so leaf nodes (94% of all nodes) carry only the 8-byte null pointer instead of a 32-byte empty `SmallVector`. Internal nodes allocate the vector on first `insert_child()` call. Access is via `child_count()`, `child_at()`, and `has_children()` helpers.

`SmallVector<T, N>` stores up to N elements inline (no heap allocation), spilling to the heap above N. This serves design principle #3 (predictable latency): node splits on the write path are zero-allocation in the common case.

### 3.2. Transient Copy-on-Write (COW) Model
*   A global `std::atomic<uint64_t>` generates unique edit tags for Transient sessions.
*   During a transient mutation, if the traversed node's `edit_tag` matches the session's tag, the node is mutated **in-place**.
*   If the tag differs (e.g., `0` or an older session), the node is **copied**, the copy is tagged with the current session ID, and the mutation applies to the copy.

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


### 4.2. Transient API (`TransientRadixTree<V>`)
Operations mutate the tree in-place utilizing the COW epoch logic.

*   `std::optional<V> get(std::span<const std::byte> key) const` *(Reads the current state of the transient tree)*
*   `bool contains(std::span<const std::byte> key) const`
*   `void set(std::span<const std::byte> key, V val)`
*   `bool erase(std::span<const std::byte> key)`
*   `PersistentRadixTree<V> persistent() &&` *(Consumes the builder; the transient must not be used after this call)*


### 4.3. Iterator API (`Iterator`)
Because keys are fragmented across nodes, the iterator must materialize the key dynamically during Depth-First Search (DFS) traversal.

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

### 5.2. Erasure / Path Compression
When removing a value (`node->value = std::nullopt`), the tree must maintain Patricia Trie invariants:
1.  **0 Children:** The node is deleted. Recursively check parent.
2.  **1 Child:** The node is merged with its child. The new prefix is `parent_prefix + transition_byte + child_prefix`.
3.  **>1 Child:** The node remains as a routing node.

---

## 6. Acceptance Criteria
1.  **Immutability:** Verifiable tests showing that `t2 = t1.set(...)` does not alter `t1`'s observable state or iterators.
2.  **Prefix Reuse:** Memory profiling or node-inspection tests proving that inserting "prefix_A" and "prefix_B" creates exactly one shared node for "prefix_".
3.  **Transient Efficiency:** Google Benchmark suite (`benchmarks/map_bench.cpp`, target `bytecask_bench`) comparing `PersistentRadixTree` against `PersistentOrderedMap` across persistent set, transient set, get, iteration, and lower_bound at 1k/10k/100k keys. Transient bulk insert must be significantly faster than chained persistent `set()` for the same container.
4.  **Ordered Iteration:** `std::is_sorted` returns true when iterating from `begin()` to `end()` comparing `iterator.key()`.
5.  **SmallVector inline storage:** Inserting keys that produce splits with prefixes ≤ 24 bytes and nodes with ≤ 8 children does not trigger heap allocations for those containers (verified via allocator instrumentation or node inspection).
6.  **Erase Compaction:** Erasing all keys from a tree results in a completely empty root, with no dangling routing nodes.
7.  **Model-based property tests:** A deterministic PRNG-driven test generates 10,000 random `set`/`erase` operations over short byte-array keys (alphabet of 6, length 1–8 for high prefix overlap) and applies them to the radix tree and a `std::map<std::string, int>` oracle. After every operation the following invariants are checked:
    - `size()` equals `oracle.size()`.
    - `get(k)` returns the same value (or `nullopt`) as the oracle for the operated key.
    - `contains(k)` agrees with `oracle.count(k)` for the operated key.
    - Every 50 rounds: full iteration yields the same key-value sequence as iterating the oracle in ascending order.
    - Every 100 rounds: `lower_bound(probe)` returns the same first key/value as `oracle.lower_bound(probe)` for a random probe key.
    - Every 200 rounds: a snapshot taken before the mutation still matches the entries captured at that point (immutability).

    The fixed seed makes any failure deterministic and reproducible; the round number in `INFO` pinpoints the failing operation without shrinking.
8.  **Memory safety:** Full test suite (70 test cases, 47,529 assertions) passes under Clang AddressSanitizer + LeakSanitizer with zero errors. Build via `xmake f --sanitizer=address -m debug`.
9.  **Concurrent reader/writer safety:** A dedicated test (`[concurrency]` tag) spawns 4 reader threads iterating a persistent snapshot while a writer thread mutates a transient derived from the same snapshot. All readers observe a consistent, unchanged snapshot throughout. ThreadSanitizer verification requires ASLR control (`xmake f --sanitizer=thread -m debug`; run with `setarch -R` or lowered `vm.mmap_rnd_bits`).
10. **Memory footprint:** `BM_MemoryFootprint` benchmarks in `map_bench.cpp` report heap bytes allocated per key for each container at 1k/10k/100k keys, enabling relative comparison of memory overhead.

---

## 7. Memory Usage

### 7.1. Node layout breakdown

Each `Node<V>` is allocated via `new` and managed by `IntrusivePtr<Node>`, which embeds the reference count inside the node itself. No separate control block is allocated.

| Field | Type | Bytes |
|---|---|---|
| `refcount_` | `atomic<uint32_t>` | 4 |
| `packed_tag_` | `uint32_t` | 4 |
| `value_` | `V` (e.g. `int`) | 4 |
| *(padding)* | | 4 |
| `prefix` size word | `size_t` | 8 |
| `prefix` union (inline 24 B or `std::vector`) | `union` | 24 |
| `children_` | `unique_ptr<ChildVec>` | 8 |
| **Node struct total** | | **56 bytes** |

Leaf nodes (94% of all nodes): 56 B, `children_` is null, no heap allocation for children.

Internal nodes (6%): 56 B + heap `std::vector` (~24 B header + N × 16 B per child slot). One child slot is `pair<byte, IntrusivePtr<Node>>` = 1 byte + 7 bytes padding + 8 bytes (`IntrusivePtr` = raw ptr) = **16 bytes**.

| Node type | Fraction | Struct | Heap children | Total |
|---|---|---|---|---|
| Leaf (0 children) | ~94% | 56 B | 0 | **56 B** |
| Internal (2 children avg) | ~6% | 56 B | ~56 B | ~112 B |
| **Weighted average** | | | | **~59 B** |

The previous design used `SmallVector<pair<byte, IntrusivePtr<Node>>, 1>` for children (32 bytes inline regardless of child count). Every node — including leaves — paid the full 32 bytes. Switching to `unique_ptr<ChildVec>` saves 24 bytes per leaf node (32 → 8).

### 7.2. Node distribution

For a typical key set with reasonable prefix compression the tree has approximately 1.07 nodes per key (a small constant overhead for routing nodes at split points). Leaf nodes (0 children) make up ~94% of nodes; internal routing nodes (~6%) spill their `children` vector to the heap because they fan out over many transition bytes (e.g. the 16 hex-digit branches of a UUID segment).

| Node type | Fraction | Children storage |
|---|---|---|
| Leaf (0 children) | ~94% | `nullptr` (0 B) |
| Internal (2+ children) | ~6% | heap `std::vector` (~24 B header + N × 16 B) |

### 7.3. Measured footprint

Values from `BM_MemoryFootprint` at 100k keys (measured with the global allocator tracker):

| Container | Key type | B/key (generic) | B/key (prefixed UUIDv7) | Key-length sensitivity |
|---|---|---|---|---|
| `PersistentRadixTree` | `span<byte>` (not stored) | **86 B** | **92 B** | Low — prefix compression absorbs shared bytes |
| `std::map` | `std::string` | 72 B | 117 B | High — full key copied into every node |
| `PersistentOrderedMap` | `immer::flex_vector` entry | ~40 B\* | ~75 B\* | High — full key copied per entry |

\* `PersistentOrderedMap` figures reflect structural sharing at the `immer::flex_vector` chunk level, not raw per-key key storage. A single live snapshot in isolation carries roughly the same key storage cost as `std::map`.

### 7.4. Prefix compression in practice

Prefix compression is not free: it replaces raw key bytes with tree structure. The trade-off is:

- **What gets compressed**: bytes shared between keys with a common prefix are stored once in a routing node rather than repeated in every leaf.
- **What gets paid**: each leaf node carries ~56 bytes of struct overhead; internal nodes carry ~56 B struct + a heap-allocated children vector. The weighted average is ~59 B/node.

For short, dissimilar keys (e.g. the generic `"key_N"` benchmark, 6–10 bytes each), compression is minimal and the per-node overhead dominates. For long, highly-redundant keys (e.g. `"user::018f6e2c-XXXX-7000-8000-XXXXXXXXXXXX"`, 45 bytes, 5 prefixes × 20k keys), RadixTree absorbs the extra key length at a modest per-key cost while `std::map` pays the full key storage, confirming that prefix compression is working correctly.

### 7.5. 100M key projections

The primary concern for ByteCask is the key directory at production scale. The table below uses 1.07 nodes/key and the measured 86 B/key at 100k as the per-node baseline.

| Configuration | B/key | 100M keys |
|---|---|---|
| Current (`IntrusivePtr`, `unique_ptr<ChildVec>`, packed tag) | **86** | **~8.6 GB** |
| + pool allocator (batch node allocations, eliminate malloc overhead) | ~70 | ~7.0 GB |
| `std::map` (no persistence, no prefix compression) | 72 | ~7.2 GB |

The original `shared_ptr`-based design measured 129 B/key (generic) and 139 B/key (prefixed). Intrusive refcounting reduced this to 108/116 B/key (−17%). The leaf node optimization (null `unique_ptr` instead of empty SmallVector) further reduced to 86/92 B/key (−20% from intrusive, −33% from original).

### 7.6. What the `OrderedMap` memory benchmark actually measures

`PersistentOrderedMap` is backed by `immer::flex_vector`, a Radix Balanced Tree of 32-element chunks. Because consecutive `set()` calls produce a chain of versions that share chunks, the allocation tracker reports only **net new bytes in the final snapshot** — bytes from discarded intermediate versions are allocated and immediately freed. The reported ~40 B/key for generic keys and ~75 B/key for prefixed keys do not represent the RAM cost of holding a single live copy of that map. A snapshot in isolation occupies approximately the same RAM as a comparable `std::map`.