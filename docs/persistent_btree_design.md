# Persistent B+ tree key directory — design

Status: **first version implemented, not yet wired into the engine**. See
[Implementation notes](#implementation-notes-first-version) for what was
built, what differs from the design below, and what was measured.
Date: 2026-09-17 (design), 2026-09-17 (first version)
Replaces, if adopted: `docs/persistent_radix_tree_design.md` and
`docs/radix_tree_epoch_reclamation_design.md` (PR #86).
Baseline for every code reference: `main` at `5297632`, and
`radix-epoch-reclamation` at `9023a58` for the reclaimer.

## Problem

The key directory is a persistent adaptive radix tree. It works and it is
well tested, but it has grown to the point where its complexity is the cost:

- `bytecaskdb/radix_tree.cppm` is 2,854 lines on `main` and 3,245 on the
  #86 branch. Five node shapes (`Node`, `Node4`, `Node16`, `Node48`,
  `Node256`), each with its own `find_child`, `child_at`, `next_child`,
  `insert_child`, `remove_child` and `clone` arm, promotion on the 5th,
  17th and 49th child and demotion with 75% hysteresis so a node near a
  boundary does not thrash.
- Prefixes are capped at 7 inline bytes, so a long key is a chain of
  routing nodes; `build_leaf_chain` and `build_routing_chain` exist to cut
  and re-cut those chains, and erase has to compress them back
  (`merge_with_child`). Iteration and `lower_bound` have to handle a key
  spread over a path of nodes, which is why `seek` is 80 lines per iterator.
- The ordinal walk on `Node256` was quadratic until a review found it;
  the fix was another cursor abstraction (`ChildCursor`, `next_child`).
- Every one of the four algorithms (`set`, `upsert`, `erase`, `merge`)
  exists twice, once persistent and once transient, and #86 moves them
  into a `BuildSession` to get the garbage sites down to one.

Each piece is justified on its own. Together they are hard to hold in one
head, and every change to reclamation, iteration or memory layout has to
be made five times.

A B+ tree has one node shape. Keys live in leaves; inner nodes hold
separators. Path copying copies a handful of nodes per write, at any key
length. This document designs that replacement with the same three
properties the radix tree has today: a persistent structure through path
copying and structural sharing, a transient that edits in place, and
epoch-based reclamation without reference counts. It states where the
B+ tree is expected to be better, where it is expected to be worse, and
how the difference is measured before the radix tree is removed.

## Learnings taken from #86

The reclamation work in #86 and its review settled several things this
design keeps rather than re-derives:

1. **Per-child reference counts are the write-path cost.** Cloning or
   releasing a node touched every child's counter, cache misses on a large
   tree. A B+ tree inner node has 50 to 150 children, so counting would be
   worse here than it was for the radix tree. Nodes carry no count.
2. **Per-epoch reclamation stalls on a long snapshot.** Freeing a retired
   list once no older version is alive lets one `db.snapshot()` pin
   everything retired after it, so memory grows with the write rate. The
   structural-sharing memory test caught it at 378 KB against 220 KB. The
   free rule is per node: a node created by version `S` and retired by
   version `R` is reachable from a live version `V` exactly when
   `S <= V < R`.
3. **Versions form a chain, and that has to be enforced.** The interval
   rule is exact only when every version above `R` derives from `R`. Two
   siblings from one base freed nodes the other still read (ASAN found it
   after the review's probe). `publish` throws on a base that already has a
   successor; `DB::resume` resets the head before deriving; a dead head
   without a successor is *retracted*, not spliced out.
4. **Publishing must allocate nothing.** The first registry (`std::map` of
   records, `std::set` of tags) made the one-version-per-key path 87%
   slower at 1k keys. Flat vectors, a handful of live versions, retired
   nodes kept in the vector the session built.
5. **Lock traffic on pin and unpin is visible.** Four percent of the
   no-sync put went to the registry mutex (#85). The B+ tree reduces the
   number of publishes (one per batch, unchanged) but not the number of
   handle copies; #85 stays a follow-up on the shared reclaimer.
6. **The node layer knows nothing about lifetime.** Node methods return
   displaced allocations; one `discard()` site in the session decides
   free-now versus retire.
7. **Iterators hold a handle** (one pin) and raw node pointers.
8. **`merge` consumes its inputs** and starts a new lineage; the engine's
   error paths take `EngineState::degraded_copy` rather than deriving a
   version; a transient nothing was written to hands back its base.
9. **A test-only accounting invariant** (`allocated - freed - parked ==
   reachable`) is what finds missed garbage sites, together with ASAN with
   leak detection over the whole suite.

`VersionChain` on the #86 branch (`radix_tree.cppm:1132-1500`) implements
2, 3, 4, 6 and 8 and depends on the tree only through a node's tag, a
subtree walk and a destroy function. This design reuses it as its own
module (`bytecask.version_chain`) with those three operations supplied by
the B+ tree node. The reclaimer is not redesigned here; what changes is
how little garbage a B+ tree write produces and how few sites produce it.

## What the engine needs from the tree

The engine and `u32_map.cppm` use this surface, and the replacement keeps
it name-for-name so the switch is a type alias:

| Used by | Operations |
|---|---|
| `DB::get`, `contains_key`, `Snapshot` | `get`, `get_ptr`, `contains` |
| `TransientEngineState::apply_*` | transient `get`, `set`, `erase`, `upsert`, `lower_bound` (range delete and range guards collect keys first, then erase: `bytecask.cppm:1727-1743`) |
| `iter_from`, `keys_from`, `riter_from`, `rkeys_from` | `begin`, `lower_bound`, `upper_bound`, `rbegin().base()`, `value_begin`, `value_lower_bound`, `value_rbegin`, `value_rlower_bound`; iterators yield `(span<const byte> key, const V&)` with the span valid until the next advance; `operator--` on the forward iterator |
| Recovery | `merge(a, b, resolve)`, persistent `erase` for tombstones, `size` |
| `PersistentU32Map<V>` | `get_ptr`, `set`, `erase`, `begin`, with `V = FileStats` and `V = std::shared_ptr<DataFile>` |
| Everything | `transient()`, `persistent() &&`, copy of a handle is an O(1) snapshot |

Two constraints follow from the value types. `KeyDirEntry` is 16 bytes and
trivially copyable. `std::shared_ptr<DataFile>` is not: the tree must
construct and destroy `V` properly, so a node clone is a loop over live
entries, not one `memcpy`. The layout below stores `V` at an aligned
address inside the node and never moves it in place, so `const V*` and
`const V&` returned to callers stay valid for as long as the node does,
which is as long as the caller's handle.

Keys are byte strings up to `Options::max_key_bytes` (default 4,096, hard
ceiling 65,535 from the u16 wire field). The layout has to admit a key of
65,535 bytes without a second storage mechanism.

## Decisions

| # | Decision | Choice | Why |
|---|---|---|---|
| D1 | Variant | B+ tree: values only in leaves, inner nodes hold suffix-truncated separators, **no sibling links** | Sibling links break structural sharing (updating a leaf would touch its neighbour). Iteration keeps a stack of `(node, index)` like today. |
| D2 | Node shape | One slotted node: 32-byte header, node prefix, `u64` slot array growing up, entry heap growing down. A leaf entry is `V + key suffix`; an inner entry is `Node* + separator suffix`. `is_leaf` picks the payload size, nothing else differs. | One layout means one search, one insert, one erase, one pack, one clone. |
| D3 | Node size | `kLeafBytes = kInnerBytes = 4096`, compile-time, two constants read from the per-node `capacity` field. A node is larger only when a single key needs it, and such a node holds exactly one entry. | 3 levels at 1M keys and 4 at 100M for 36-byte keys; every level removed is a DRAM round trip removed from every read. Smaller inner nodes (1 or 2 KiB) and 2 KiB leaves are the alternatives to benchmark (§Performance, §Plan step 4). |
| D4 | Node prefix | Every key in a node shares `prefix_len` bytes, stored once. Recomputed on split and rebuild from the first and last key. | What keeps structured keys dense (§Memory) and makes heads discriminating. |
| D5 | In-node search | 4-byte key heads in the high half of each `u64` slot; lower bound = count of slots below `head << 32`, a branch-free loop clang vectorises; ties resolved by full compare. | Avoids touching the heap for most keys and avoids branch mispredicts. No intrinsics unless `-Rpass=loop-vectorize` shows the loop is scalar. |
| D6 | Deletion | Lazy: no rebalancing. An emptied node is unlinked and freed; the root collapses when it has one child. | A B+ tree stays correct at any fill. Sibling merge is a follow-up gated by the churn memory tests. |
| D7 | Reclamation | `VersionChain` from #86, lifted into its own module: chain contract, per-node interval rule, retraction, allocation-free publish. Version tag stamped in the node header at creation. | Reviewed, ASAN/TSAN clean, shape-agnostic. |
| D8 | Transient | One `BuildSession` per transient: a node is owned iff `tag == session tag`; owned nodes are edited, rebuilt or split in place and freed at once when replaced; foreign nodes are cloned and retired. Discard without publish frees created nodes. | Same rules as #86 with fewer sites (§Garbage). |
| D9 | Recovery merge | Consuming `merge(a, b, resolve)` as a two-way ordered merge into a fresh lineage with an append builder, for API parity. A range-parallel k-way merge follows if the recovery gate fails. | Subtree reuse across trees is what made the radix merge complex; sequential rebuild is O(n) with a hot working set. |
| D10 | Iterators | Stack of `(const Node*, index)` sized to the tree height, a handle pin, a key buffer rebuilt on advance (prefix + suffix). Value iterators skip the buffer. | Same contract as today: a span valid until the next advance, no dangling. |
| D11 | Persistent one-shot `set`/`erase` | `transient().set().persistent()` | One code path; tests and `u32_map` use these. |
| D12 | Portability | `Node*` is 4 bytes on wasm32 and the layout adapts through `sizeof`/`alignof`; slots are `u64` words built with shifts, so the search does not depend on endianness. `BYTECASK_SINGLE_THREADED` is unchanged. | The WASM backend builds this module too. |

## Node layout

```
                     capacity bytes (kNodeBytes, or exactly what one giant key needs)
  ┌──────────────┬─────────────┬────────────────────────┬──── free ────┬──────────────────────┐
  │ header 32 B  │ prefix      │ slots: count × u64  →  │              │ ←  entries (heap)    │
  └──────────────┴─────────────┴────────────────────────┴──────────────┴──────────────────────┘
                 ^ 32          ^ align8(32 + prefix_len)                ^ end - heap_floor   ^ end

  slot   = head << 32 | len << 16 | off          head: first 4 suffix bytes, big-endian, zero-padded
                                                  len:  suffix length
                                                  off:  distance from the node end to the entry end
  entry  = [payload][key suffix]                  payload: V (leaf) or Node* (inner)
           entry start is aligned to alignof(payload); entry size rounds up to that alignment
```

```cpp
struct NodeHeader {                 // 32 bytes
  std::uint64_t tag;                // version that created the node; ownership test in a
                                    // transient, window start for reclamation
  Node *first_child;                // inner only: child left of every separator
  std::uint32_t capacity;           // bytes allocated
  std::uint16_t count;              // entries: keys in a leaf, separators in an inner node
  std::uint16_t prefix_len;         // bytes every key in this node shares; stored once
  std::uint16_t heap_floor;         // distance from the node end to the lowest live entry
  std::uint16_t dead_bytes;         // bytes of erased or replaced entries; reclaimed by pack()
  std::uint8_t is_leaf;
};
```

Invariants, checked by a debug `validate()` and by the tests:

- Slots are sorted by full key (prefix + suffix). Heads are consistent with
  the suffix bytes.
- `prefix_len` is at most the common prefix of the first and last key.
  It may be shorter; it is only ever extended by `pack()`.
- An inner node with `count` separators has `count + 1` children:
  `first_child`, then the payload of each entry. The subtree at child `i`
  holds keys in `[sep_i, sep_{i+1})`, with `sep_0 = -inf`.
- A leaf holds at least one key; an inner node at least one separator,
  except the root, which may be an empty leaf. A single-child inner node
  can exist below the root after deletions; it costs one hop and is
  unwound when its last child goes. All leaves are at the same depth.
- `capacity > kNodeBytes` implies `count == 1`. Such a node has no slack,
  so the next insert into it splits. This is why `off` is measured from
  the node end to the entry *end*: a 65,535-byte key's entry starts more
  than 65,535 bytes from the node end, but ends at it.
- Nodes are immutable once their version is published. A node is written
  only by the session whose tag it carries.

Sizes for `V = KeyDirEntry` (16 B) on a 64-bit host: an entry is
`align8(16 + suffix)`, a slot 8 B. A 4 KiB leaf holds about 60 keys of
36 bytes, about 100 of 8 bytes. An inner entry is `align8(8 + separator)`;
separators are short (§Split), so inner fanout is 100 to 150.

### Search within a node

`head` is the first four suffix bytes as a big-endian integer, zero-padded
for shorter suffixes. For two suffixes `a` and `b`, `head(a) < head(b)`
implies `a < b`, and `head(a) > head(b)` implies `a > b`; equal heads decide
nothing. So the lower bound of a target in a node is:

```cpp
// Under the node prefix has already been matched.
auto lower_bound_in(const Node &n, std::span<const std::byte> suffix) -> Position {
  const auto target = std::uint64_t{head_of(suffix)} << 32;
  std::size_t pos = 0;
  for (std::size_t i = 0; i < n.count; ++i)     // branch-free; vectorised by clang
    pos += n.slots[i] < target;
  for (; pos < n.count && head(n.slots[pos]) == head_of(suffix); ++pos) {
    const auto cmp = compare(n.suffix_at(pos), suffix);
    if (cmp >= 0) return {pos, cmp == 0};
  }
  return {pos, false};
}
```

The first loop reads 8 bytes per key from one contiguous array: 480 bytes
for a 60-key leaf. Clang vectorises `pos += slot < target` at `-O3` with
`-march=native` (AVX2: four slots per compare; NEON and WASM SIMD128: two).
The tie loop touches the heap only for keys whose first four suffix bytes
equal the target's, which under a node prefix is the target itself and its
immediate neighbours. This is the Umbra/LeanStore "heads" technique with
the head packed into the slot word so no gather is needed.

The implementation ships the scalar loop and a build check that it was
vectorised. Explicit intrinsics are out of scope until a profile shows the
loop as a cost.

Routing in an inner node uses the same function: with `(pos, exact)` for
the key, the child index is `pos + 1` if `exact` else `pos`.

### Comparing against the node prefix

A lookup descends with `remaining = key`. At each node it compares
`remaining[0..prefix_len)` with the stored prefix. If they match, it
searches the node with `remaining.subspan(prefix_len)`. If they differ at
byte `i`, every key in the node is on one side of the target: for a
lookup that is "absent"; for `lower_bound` the position is `0` or `count`
by the sign of the first differing byte; for an inner node the child is
`first_child` or the last child. This is the same three-way case the
radix tree's `seek` has, once per node instead of once per prefix chunk.

## Algorithms

Every algorithm is written once, inside `BuildSession`, and every
persistent operation is a one-node session (D11). The descriptions below
are for a transient; the persistent variant is the same code with
`persistent()` at the end.

### get

Descend from the root: prefix check, `lower_bound_in`, child. At the leaf,
`exact` decides. No allocation, no atomics; the caller's handle keeps the
nodes alive.

### set (insert or overwrite)

1. Descend, recording the path `(node, child index)`.
2. Make the leaf mutable: `mutable_copy(leaf)` returns `leaf` itself if the
   session owns it, otherwise a clone with the session's tag, and retires
   the source. Then the same for each ancestor whose child pointer has to
   change, bottom-up. In a transient that touches the same leaf again the
   whole path is owned and nothing is copied.
3. Overwrite: if `exact`, replace the payload in place (destroy the old
   `V`, construct the new one at the same address). The key bytes do not
   change. Size unchanged.
4. Insert: if the key does not share the node prefix, `rebuild` with the
   shorter prefix (§Rebuild). If the entry and its slot fit in the free
   space, write the entry at `heap_floor`, shift the slots above `pos` up
   by one word, write the slot. If they do not fit but would after
   compaction (`dead_bytes` large enough), `rebuild`, then insert. Else
   `split` (below), insert into the correct half, and insert the separator
   and the right half into the parent, which is the same procedure one
   level up. A root split allocates a new root with one separator.

The append case matters for recovery and for ascending workloads: when the
insert position is `count` (the new key is greater than every key in the
leaf) and the leaf is the rightmost leaf of the tree, `split` moves only
the new key into the right half instead of half the bytes. Leaves built by
ascending inserts are then full instead of half full. The rightmost test
is one comparison per path level (child index is the last on every
frame).

### Rebuild (pack)

`pack(src, prefix_len) -> Node*` allocates a node of the same capacity,
writes the prefix, and copies the live entries in slot order, constructing
each `V` in place, recomputing heads if the prefix changed, and leaving
`dead_bytes = 0`. It is the one function behind three needs: a clone of a
foreign node (a clone with its holes already compacted), compaction of an
owned node (pack, free the old allocation), and a prefix change (pack with
the new `prefix_len`). Cost is O(bytes in the node).

### Split

Choose the split index so that the live bytes on each side are balanced,
with at least one entry on each side. Left and right are packed fresh, each
with its own prefix, which is the common prefix of its first and last key.
The separator is the shortest string greater than the last key of the
left half and not greater than the first key of the right half:

```
cpl = common_prefix_length(left.last_key, right.first_key)
separator = right.first_key[0 .. cpl + 1)
```

This is suffix truncation. Separators are as short as the keys allow, so
inner nodes are small and the head of a separator is almost always
decisive. The old node is freed if owned, retired if foreign.

### erase

Descend; if absent, nothing changes. Make the path mutable. Destroy the
`V`, shift the slots down, add the entry size to `dead_bytes`. If the leaf
is now empty: free it (it is owned) and remove its child pointer from the
parent, which drops the separator to its left, or, for `first_child`, the
first separator and promotes that separator's child to `first_child`. An
inner node that becomes empty is removed the same way. If the root has one
child, the child becomes the root.

Nothing else. No rebalancing, no borrowing. A tree that has had half its
keys deleted at random is half full and stays correct; inserts refill it.
The `[memory]` churn tests decide whether a sibling merge is needed later.

### upsert

Single descent with the predicate at the leaf, as today: insert if absent,
replace if `should_replace(existing, incoming)`, return the displaced
value. Used by recovery to avoid a second descent.

### lower_bound, upper_bound, iteration

`lower_bound(key)` descends like `get` but records the path; at the leaf
the position is the first key `>= key`; if that is `count`, the iterator
advances to the next leaf through the stack. `upper_bound` is
`lower_bound` plus one step on an exact match. Forward advance is
`++index` within the leaf, else pop until a frame has a next child and
descend leftmost. Backward is symmetric. The reverse iterator wraps a
forward iterator and pre-decrements, exactly as `ReverseRadixTreeIterator`
does now, so the span it yields is into a live buffer.

The key buffer is `prefix + suffix` of the current entry, rebuilt on every
step. That is a copy of one key, up to 4 KiB and usually under 64 bytes,
against today's per-node `push_back`/`resize` along a path.

Value iterators (`value_begin`, `value_lower_bound` and reverse) are the
same stack without the buffer, returning `const V&` into the node.

### merge (recovery fan-in)

`merge(a, b, resolve)` consumes both inputs (#86's contract) and builds a
new lineage: two forward iterators, an ordered merge with `resolve` on
equal keys, and an append builder that fills leaves to capacity and stacks
inner levels as it goes. The result shares no node with either input, and
the inputs are freed when their handles are dropped.

The engine merges partitions pairwise, `log2(W)` levels deep
(`recovery_merge_results`, `bytecask.cppm:3640`). With sequential rebuild
the critical path is the last merge, all 10M keys on one thread, about
1 s at 100 ns per key. That misses the recovery numbers in the README
(0.51 s at 10M with 16 threads). The follow-up, if the gate in §Plan
demands it, is one range-partitioned k-way merge: sample `T` split keys
from the partition trees, have each of `T` threads merge its range from
all `W` partitions into a subtree with the append builder, and join the
subtrees under one root, padding shorter subtrees with single-child inner
nodes, which the invariants already allow. Each thread then does `10M/T`
appends; at 16 threads that is under 100 ms. This changes
`recovery_load_parallel`, not the tree.

## Versions, transients and reclamation

A `PersistentBTree<V>` handle is `(root, size, version)`; copying it pins
the version in the `VersionChain`, destroying it unpins. `transient()` on
a handle starts a `BuildSession` with the next tag of the lineage and a
copy of the base handle, so the base cannot die under the session.
`persistent() &&` publishes: the chain checks that the base has no
successor, records the new version, parks the session's retired nodes on
the live versions that still reach them, and frees the rest. A transient
nothing was written to returns the base handle and publishes nothing.

### Where a node becomes garbage

There are three sites, all in `BuildSession`, against four in #86:

1. **Superseded.** `mutable_copy` clones a foreign node: the source is
   retired.
2. **Replaced.** `rebuild` or `split` replaces a node: the old allocation is
   freed at once if owned, retired if foreign. (`split` of a foreign node
   is `mutable_copy` then `split`, so in practice a split always replaces
   an owned node.)
3. **Never published.** A session destroyed without `persistent()` frees
   every node it created, found by walking from its root and stopping at
   any node whose tag is not the session's, since a child is never newer
   than its parent. Its retired list is dropped.

The fourth site in #86, a dropped subtree, does not exist: a B+ tree
erase unlinks only nodes that are empty, and an emptied node is owned
because the path was made mutable before the erase. Every node this tree
ever unlinks is freed immediately.

Per single-key write the session retires the nodes on one path, three or
four, and creates their clones. That is the whole cost reclamation has to
absorb, and it is what makes the parked lists in the chain short.

### The chain, unchanged

`VersionChain` keeps its two contracts and one free rule from #86: a
version may be derived only from a version with no successor; `merge`
consumes sole-handle lineages; a node created by `S` and retired by `R` is
freed once no live version of its lineage has a tag in `[S, R)`;
retraction of a dead successor-less head frees what the dead segment
created (a walk from the dead root, stopping at nodes with a tag at or
below the newest live predecessor) and unparks what it retired.

The chain needs three things from the node type and gets them as a small
traits interface: `tag(node)`, `for_each_child(node, f)` (inner nodes only)
and `destroy(node)` (which destroys the `V`s of a leaf, then frees). The
module moves from `radix_tree.cppm` to `version_chain.cppm` with those as
template parameters and no other change. Its `[accounting]` tests come
with it.

The engine change that goes with the chain (`DB::resume` resets `head_`
to the published state before deriving) is part of #86 and lands with it.

What the B+ tree removes from the node: the retired bit and the atomic tag
word are already gone on the #86 branch; here the tag is a plain `u64` in
the header, written once at creation.

### Concurrency

Single writer, lock-free readers, as today. A reader holds an `EngineState`
whose `key_dir` handle pins one version; every node it can reach is
immutable and will not be freed while the pin is held. Publication order
is the engine's: the tree is complete before `state_.store()` (release),
readers `load` (acquire). Reclamation runs on whichever thread drops the
last pin of a version, under the chain's mutex, with the frees outside it,
unchanged.

The transient is used by one thread. Its iterators hold raw node pointers
and must not be alive across a mutation of the same session; the engine
already collects keys before erasing.

## Memory

Per key, for `V = KeyDirEntry` and a 4 KiB node: one slot (8 B), the
payload (16 B), the key suffix, and 3.5 B of alignment on average, divided
by the fill factor; the header is under 1 B per key. Fill is about 0.69
for random-order inserts (the classic B-tree result) and 0.9 or better for
ascending inserts with the append split.

The table applies that to the shapes `benchmarks/memory_profile.cpp`
measures, with the node prefix taken as what about 50 adjacent keys share.
The radix column is the measured RSS per key at 1M keys from
`docs/persistent_radix_tree_design.md` §7.6, which includes the engine's
fixed overhead, so it is somewhat generous to the radix tree. Both are
estimates until step 4 of the plan runs `memory_profile` on the real thing.

| Key shape | Key | Suffix in node | B+ tree est. | Radix measured | Ratio |
|---|---:|---:|---:|---:|---:|
| incremental (ascending) | 4 | 2 | 33 | 54 | 0.6 |
| uniform `key_N` | 8 | 3 | 44 | 54 | 0.8 |
| binary | 8 | 5 | 47 | 65 | 0.7 |
| uuidv7_binary | 16 | 10 | 54 | 51 | 1.1 |
| clustered | 16 | 5 | 47 | 83 | 0.6 |
| many_partitions | 13 | 9 | 53 | 105 | 0.5 |
| hash_prefixed | 24 | 8 | 51 | 60 | 0.9 |
| zipfian | 27 | 12 | 57 | 60 | 1.0 |
| uuidv7 text | 36 | 22 | 72 | 60 | 1.2 |
| prefixed uuidv7 | 44 | 25 | 76 | 50 | 1.5 |
| uuidv4_binary | 16 | 14 | 60 | 181 | 0.3 |
| sha256_bin | 32 | 30 | 83 | 409 | 0.2 |
| uuidv4_text | 36 | 33 | 88 | 434 | 0.2 |
| uuidv4_prefixed | 44 | 35 | 91 | 464 | 0.2 |
| sha256_hex | 64 | 60 | 127 | 881 | 0.15 |

Three conclusions:

- For random keys the B+ tree is three to seven times smaller. The radix
  tree pays a routing node per diverging byte; the B+ tree pays the key
  bytes once. This is the case where the radix tree is unusable today
  (840 MB for 1M SHA-256 hex keys).
- For short and moderately structured keys the two are within 20% either
  way, with the B+ tree ahead on wide-fanout shapes (`clustered`,
  `many_partitions`) where the radix tree's `Node48`/`Node256` tiers cost
  it.
- For long keys with structure shared deeper than one node's prefix
  (`prefixed uuidv7`, `uuidv7` text) the radix tree is 20 to 50% smaller.
  A node prefix captures the first level of sharing only. This is the
  honest cost of the change; the README's "~50 bytes per key" becomes
  "~50 to 75 bytes per key" for those shapes and the RAM sizing paragraph
  is updated accordingly.

Not counted: the retired nodes waiting on a live snapshot, which the
interval rule bounds by what the snapshot reaches, as today.

## Performance expectations

Stated before measuring so the benchmark can disagree.

**Point lookup.** The cost of a lookup is its chain of dependent cache
misses, not the bytes it touches. The B+ tree's chain is three nodes at 1M
keys and four at 100M. The top two levels are a few hundred to a few
thousand nodes touched by every operation, so they stay in cache; what
goes to DRAM is the leaf and, at 100M, the level above it. Inside a node
the slot array is contiguous and prefetched, so a level costs about two
dependent misses: the slots, then the one entry the head selected. That
is 2 to 4 DRAM round trips per `get`. The radix tree's chain for a
36-byte text key is 5 to 12 nodes (one per 7-byte prefix chunk plus one
per branch), each a serialized miss into a node allocated nowhere near
its siblings; its cached upper half does not shorten the cold lower half.
Expect `Get` equal or better; the tree is a minor part of the 728 ns p50,
most of which is the `pread`.

**Single-key commit.** The path copy is three or four packed nodes of
about 2.8 KiB each. The bytes are not the cost: a 2.8 KiB memcpy from
cache is about 30 ns. The cost is the cold leaf, 44 contiguous lines the
prefetcher streams from DRAM in 150 to 200 ns, plus three or four
allocations. The radix tree pays the same random write as five to eight
small clones, each a dependent miss into a cold node: the same 300 ns or
more, spent in a chain of misses instead of a streaming copy. Expect
`Put/NoSync` within noise and `Put/Sync` (7 ms per op) unaffected. The
gate allows −5% on `Put/NoSync` as the bound for acting, not as the
prediction.

Where the size constants matter is the copy volume per commit, which is
`depth × fill × node size`, for 36-byte keys and fill 0.69:

| Leaf / inner size | Depth 1M / 100M | Copied per commit 1M / 100M |
|---|---|---|
| 4 KiB / 4 KiB (default) | 3 / 4 | 8.5 / 11.3 KiB |
| 4 KiB / 2 KiB | 4 / 5 | 7.0 / 8.4 KiB |
| 4 KiB / 1 KiB | 4 / 6 | 4.9 / 6.3 KiB |
| 2 KiB / 2 KiB | 4 / 5 | 5.6 / 7.0 KiB |
| 2 KiB / 1 KiB | 5 / 6 | 4.2 / 4.9 KiB |

Copy volume scales as `size × log(N) / log(size)`, so smaller nodes help
slowly, and every step adds one level to every read. The inner size is
the cheaper lever: inner nodes are about 2% of nodes, so memory does not
move, and `capacity` is already a per-node header field, so a second
constant is not a second code path. The leaf size is the lever for
batched random writes (below). Both stay constants and the step 4 matrix
decides; the default is the read-first choice.

**Allocation.** Nodes are allocated with plain `new` and freed with
`delete`. A bounded freelist of whole nodes fed by reclamation (LMDB's
page freelist, in memory) is the first thing to try if `Put/NoSync`
misses its gate and `perf` attributes it to `malloc`/`free`, ahead of
changing node sizes. It is not in the design until that measurement
asks for it. An arena shared below node granularity is ruled out: it
would bring per-entry lifetime back.

**Batched writes.** After the first touch a node is owned and edited in
place with a slot shift and an entry write. Inner nodes are copied once
per batch; a batch of random keys copies one leaf per distinct leaf
touched, so the per-key cost is one cold leaf regardless of batch size,
and the leaf size is what halves it. A batch of nearby keys copies almost
nothing. Expect `MixedBatch`, `PutMT/Sync` and sysbench
within noise of #86, and `TransientSet` in `map_bench` faster: no node
promotion, no chain splitting.

**Range scans.** Keys inside a leaf are adjacent, one contiguous block
read sequentially, with one hop per 60 keys. The radix DFS visits at
least one scattered node per key. `Range50` gained 43% on the #86 branch
from removing refcount traffic alone; the B+ tree removes the walk.
Expect `Range50` and `Iterate` well ahead, `LowerBound` ahead by the same
argument as `Get`.

**Recovery.** Partition build per thread is an insert workload, roughly as
today. The fan-in merge is the risk (§merge) and has its own row in the
gate.

**Latency shape.** No operation does work proportional to fanout beyond a
memcpy of one node, and a split is bounded by two node packs. The
predictable-latency principle holds at least as well as today, where a
`Node256` clone is a 2 KiB copy.

## What changes in the engine

- `bytecaskdb/btree.cppm` (module `bytecask.btree`), new:
  `PersistentBTree<V>`, `TransientBTree<V>`, `BTreeIterator<V>`,
  `ReverseBTreeIterator<V>`, `BTreeValueIterator<V>`,
  `ReverseBTreeValueIterator<V>`, `BuildSession<V>`.
- `bytecaskdb/version_chain.cppm`, lifted from the #86 branch.
- `internals.cppm`: `EngineState::key_dir` and `RecoveryResult::key_dir`
  through one alias, `KeyDir = PersistentBTree<KeyDirEntry>`, and the same
  for its transient; `bytecask.cppm` uses the aliases. `u32_map.cppm`
  wraps the B+ tree; fixed 4-byte big-endian keys mean the head covers the
  whole key and the tie loop never runs.
- Recovery: `recovery_merge_results` unchanged in step 2; the
  range-parallel merge in step 3 if needed.
- `bytecaskdb/radix_tree.cppm`, its tests and its two design documents
  are removed in the last step, once the gates pass. Until then both trees
  build and the suite runs against the alias.
- Documentation: the key directory section of `docs/bytecask_design.md`,
  the README's opening paragraph (radix tree, per-key RAM figure) and its
  documentation table.

Not changed: the on-disk format, hint files, `EngineState` publication,
the commit pipeline, snapshots, the public API.

## Size estimate

| Part | Lines |
|---|---:|
| Node layout, prefix, slot search, pack, split | 350 |
| `BuildSession`: get, set, upsert, erase, root handling, discard | 400 |
| Handles, transient, publish, traits for the chain | 150 |
| Iterators, four kinds | 350 |
| Append builder and merge | 150 |
| **`btree.cppm`** | **~1,400** |
| `version_chain.cppm` (moved, not new) | ~400 |

Against 2,854 lines on `main` and 3,245 on the #86 branch for the radix
tree with its reclaimer inline. The count is a gate (§Plan), not a hope:
if the module lands above 2,000 lines something from the old design has
crept back and the review asks what.

## Tests

- Port `tests/radix_tree_test.cpp` (87 cases) and
  `tests/radix_tree_memory_test.cpp` (22 cases) to the new module; the
  behaviours they assert are the contract. Port the eight `[accounting]`
  cases from #86 with the chain.
- New B+ tree cases: split at every position including the append case;
  root split and root collapse; prefix shrink on insert and prefix growth
  on split; keys shorter than four bytes and keys equal in their first
  four suffix bytes (the tie loop); embedded zero bytes against zero
  padding; a 65,535-byte key alone in its node and the split that follows;
  empty key; erase to empty through several levels; a single-child inner
  node surviving and unwinding; `validate()` after every operation in a
  random model test against `std::map`.
- The `[model]` engine tests (random, batch-heavy, delete-heavy) under
  serial and parallel recovery, unchanged: the tree cannot make them
  diverge, but they exercise `merge` and `upsert`.
- Sanitizers: full suite under ASAN with `detect_leaks=1` and under TSAN
  (`scripts/run_sanitizer.sh`).
- Node size and vectorisation: a build check that
  `-Rpass=loop-vectorize` reports the slot loop, on x86-64 and arm64.

## Benchmarks and gates

Run per the repository rules: `python3 scripts/run_map_bench.py`
(with the `RadixTree/*` names kept as `KeyDir/*` so history stays
comparable), `python3 scripts/run_engine_bench.py --full`,
`scripts/run_memory_profile.py`, and the sysbench pair, interleaved
against the baseline (`main` after #86 merges).

| Gate | Pass |
|---|---|
| G1 | Full suite green; ASAN with leak detection and TSAN clean; accounting invariant holds |
| G2 | `memory_profile`: every shape within 15% of the estimate table, none above 1.5× the radix figure |
| G3 | `engine_bench` 1M: `Get` and `Range50` not worse; `Put/Sync`, `Del/Sync`, `MixedBatch/Sync`, `PutMT/Sync` 2–16 threads within ±3%; `Put/NoSync` not worse than −5% |
| G4 | Recovery 1M and 10M at 1/4/16 threads within ±10% (step 3 decides whether the parallel merge is needed) |
| G5 | `map_bench`: `TransientSet`, `Get`, `LowerBound`, `Iterate` not worse; `PersistentSet` within ±10% |
| G6 | sysbench oltp_insert and oltp_write_only not worse than the #86 branch |
| G7 | `btree.cppm` under 2,000 lines, no per-node-type dispatch anywhere |

If G3's `Put/NoSync` fails: first a node freelist if `perf` shows
`malloc`/`free`, then a smaller inner node. If G2 fails: 2 KiB leaves.
Each is one constant or one small class, tried one at a time.

## Plan

Each step is a reviewed commit with the suite green.

| Step | Work | Done when |
|---|---|---|
| 0 | Review this document; settle the open questions below. Merge #86 first: its engine changes and `VersionChain` are the base. | Design approved, issue filed |
| 1 | `version_chain.cppm` lifted out with node traits; `btree.cppm` with layout, search, `BuildSession`, handles, iterators, append builder, `merge`; ported and new tests; accounting tests. Radix tree untouched. | Tree tests and `[accounting]` green under ASAN/TSAN |
| 2 | `KeyDir` alias in `internals.cppm`; `u32_map` on the B+ tree; engine on the alias. | Full suite, `[model]`, sanitizers green (G1) |
| 3 | Recovery: measure fan-in; implement the range-parallel merge if G4 fails. | G4 |
| 4 | Benchmarks and memory profile against the baseline; the 2 KiB node comparison; record rows in the CSVs and here. | G2, G3, G5, G6 |
| 5 | Remove `radix_tree.cppm`, its tests, benchmarks adapters and the two radix design documents; update `bytecask_design.md`, README, intro; close #84, #85 (or re-scope to the chain), #86's follow-ups. | G7; docs match the code |

## Open questions

1. **Node sizes.** 4 KiB leaves and 4 KiB inner nodes are proposed for
   read depth; 4K/1K and 2K/1K are in the step 4 matrix, judged on `Get`
   at 10M keys and batched random writes together. Accept the default
   with the comparison in step 4?
2. **Lazy deletion.** No sibling merge in the first version. Accept, with
   the churn memory tests as the trigger for adding it?
3. **Order with #86.** Merge #86 first and build on its chain (proposed),
   or keep #86 open and lift `VersionChain` from the branch directly? The
   first gives the engine its gains now and a clean baseline for the
   gates.
4. **Recovery merge scope.** Sequential consuming merge in step 2 and the
   parallel merge only if G4 fails (proposed), or design the parallel merge
   in from the start?
5. **README figure.** The per-key RAM paragraph will read "50 to 75 bytes
   per key for structured keys, 60 to 130 for random keys" instead of the
   current "~50". Acceptable framing?

## Implementation notes (first version)

Built on branch `claude/btree-design-bytecaskdb-vqjnzm`, tracked in #102.
The tree is exercised through its own tests, `map_bench` and an index-only
mode of `memory_profile`; the engine still uses the radix tree.

| Part | Where | Lines |
|---|---|---:|
| Tree: node layout, search, `BuildSession`, handles, iterators, merge | `bytecaskdb/btree.cppm` | 1,616 |
| Reclaimer, lifted from #86 and abstracted over the node type | `bytecaskdb/version_chain.cppm` | 487 |
| Tests: 14 cases, 790k assertions, clean under ASAN with leak detection | `tests/btree_test.cpp` | 554 |

`btree.cppm` is under the 2,000-line gate (G7), with about 250 of its
lines being the debug `validate()`, `stats()` and `visit_nodes()` support.
There is no per-node-type dispatch anywhere.

### What differs from the design above

- **Slot and entry layout.** A slot is `head << 32 | offset` with a 32-bit
  offset from the node end to the entry *start*, and the entry carries its
  own 16-bit suffix length: `[payload][u16 len][suffix]`. The design's
  16-bit offset could not address a 65,535-byte key's entry, and measuring
  from the entry end instead would have made the entry unparseable without
  its length. Cost: 2 bytes per key. The u16 length is the reason the tree
  rejects keys above 65,535 bytes with `std::length_error`, the same
  ceiling as the data file's `key_size` field.
- **Oversized nodes hold as many entries as fit.** With 32-bit offsets the
  "a node larger than `kNodeBytes` holds exactly one entry" rule is
  unnecessary. A rebuild with a shorter prefix or a split among giant keys
  allocates whatever the entries need, with no slack, so the next insert
  into it splits.
- **Split point.** Three rules, in order (`BuildSession::split`):
  1. an entry at either end that alone shortens the node prefix by 8 bytes
     or more is split off on its own;
  2. an insert that continues the previous in-place insert into the same
     node (the header records `last_pos`) splits at the insert point;
  3. otherwise balanced by bytes.
  Rule 2 replaces the design's "rightmost path" rule and covers it. Both
  rules exist because of what the memory profile showed, below.
- **Clone is two memcpys** when the value type is trivially copyable, the
  prefix is unchanged and the node has no dead bytes (the common path copy).
  Otherwise `pack()` copies entry by entry. This is lever 4 from the write
  path analysis and it turned the single-version insert from 3× slower than
  the radix tree to faster.
- **`merge` is non-consuming.** It rebuilds a new lineage from two
  iterators and leaves the inputs alone; since nothing is shared with them,
  the consumption contract from #86 is not needed for it. It inserts through
  the ordinary `set`, so each key costs a descent; a dedicated append
  builder and the range-parallel recovery merge are still to do.
- **In-node search** samples 16 heads per node into a `hints` array in the
  header (64 bytes, the LeanStore technique the design mentions): the
  hints locate the run of about `count/17` slots that can hold the key,
  and only that run is scanned with the branch-free count (clang
  vectorises it at width 4, interleaved 4). `perf` had put the full-array
  scan at 35% of a lookup and 25% of an insert; a binary search over the
  heads traded it for branch misses and measured neutral, the hints cut
  lookups by 10 to 30%. A word-sized compare for short suffixes was tried
  in the same round and made lookups 3× slower at small sizes; reverted.
- **Split rules** grew from the two in the design to: outlier at either
  end alone; sequential insert splits at the insert point, and hands a
  foreign tail after the insert point its own node; balanced otherwise.
  `last_pos` survives rebuilds. See the fill story below.
- **Non-trivially-copyable values** (`std::shared_ptr<DataFile>` in the
  file registry) are supported: entries are copy-constructed and destroyed
  in place, never moved bitwise.

### Memory, index only, 1M keys, `KeyDirEntry` values

`BC_INDEX_ONLY=btree|radix ./memory_profile` builds only the tree from a
key shape, one transient per 100 keys as the engine does, and reports the
jemalloc heap delta. Bytes per key:

| Key shape | Key | Fill | Leaf prefix / suffix | B+ tree | Radix | Ratio |
|---|---:|---:|---:|---:|---:|---:|
| uniform `key_N` | 5–10 | 0.60 | 7.3 / 2.8 | 57 | 48 | 1.18 |
| incremental | 1–7 | 0.60 | 3.3 / 2.7 | 56 | 48 | 1.17 |
| uuidv7 text | 36 | 1.00 | 11.9 / 24.1 | 58 | 52 | 1.12 |
| binary | 8 | 0.71 | 3.9 / 4.1 | 47 | 45 | 1.05 |
| prefixed UUIDv7 | 42 | 0.99 | 41.5 / 2.5 | 34 | 45 | 0.76 |
| many_partitions | 13 | 0.67 | 4.6 / 8.4 | 63 | 83 | 0.76 |
| zipfian | 5–27 | 0.95 | 23.3 / 4.1 | 35 | 53 | 0.66 |
| hash_prefixed | 24 | 0.99 | 20.9 / 3.1 | 33 | 53 | 0.63 |
| clustered | 16 | 0.99 | 13.3 / 3.4 | 33 | 62 | 0.53 |
| uuidv7_binary | 16 | 0.99 | 5.0 / 11.0 | 42 | 80 | 0.52 |
| uuidv4_binary | 16 | 0.70 | 1.0 / 15.0 | 71 | 139 | 0.51 |
| sha256_bin | 32 | 0.70 | 1.0 / 31.0 | 96 | 299 | 0.32 |
| uuidv4_text | 36 | 0.70 | 2.8 / 33.2 | 96 | 313 | 0.31 |
| uuidv4_prefixed | 42 | 0.70 | 10.2 / 33.8 | 96 | 333 | 0.29 |
| sha256_hex | 64 | 0.70 | 2.9 / 61.1 | 133 | 628 | 0.21 |

The radix column is the same index-only measurement, so the two are
directly comparable (the estimate table earlier in this document used RSS
figures from the radix design document, which are not).

The first build measured `prefixed` at 127 B/key with leaves 28% full,
and the fix took three rounds, each found by printing the leaf-size
histogram and, in the end, logging every split:

1. The leaf receiving one key family's ascending stream also holds the
   first key of the next family, so its prefix is empty, its entries three
   times larger, and it fills at 56 keys; every balanced split then leaves
   a 32-key half behind. Split rules 1 and 2 (outlier isolation, split at
   the insert point on sequential inserts) took this to 63% fill.
2. A prefix-shrink rebuild (the hex counter rolling a digit) produced a
   fresh node that had forgotten `last_pos`, so the next split on it was
   balanced. `rebuild` now preserves `last_pos`; the entries and their
   order are unchanged by a rebuild, so the evidence is still valid.
3. The split log then showed the real residue: a leaf of 55 keys splitting
   at position 31, thousands of times. A 24-key tail of the *next* family
   was travelling along in the right node of every sequential split, and
   each split abandoned a 31-key leaf. Rule 2 now checks whether the
   entries after the insert point share far less with the new key than its
   predecessor does, and if so gives them a node of their own. `prefixed`
   went to 99% fill and 34 B/key, and `hash_prefixed`, `zipfian` and
   `clustered`, which have the same structure, from 47–65 to 33–35.

These shapes are the workload of several tables growing at once, which
is what the MariaDB plugin does, so they had to be fixed in the split
rather than dismissed as benchmark artefacts. A consequence to know about:
a leaf filled by a sequential stream is 100% full, so a later random
insert into it splits at once, as in any B-tree without a fill reserve.

What remains on the structured shapes is the entry floor: a 16-byte value,
a 2-byte length and a suffix, rounded to 8, plus an 8-byte slot, is 32
bytes per key before fill, against the radix tree's 40-byte leaf that
carries no key bytes at all for a compressed suffix. Fill of 0.60 on the
mixed-order shapes (`uniform`, `incremental`: numeric insert order is not
lexicographic order) is the other factor; random order gives 0.69.

### `map_bench`, B+ tree over radix tree on `main`, before the search hints

| Benchmark | N | RadixTree | B+ tree | B+ / Radix |
|---|---:|---:|---:|---:|
| Get | 1000 | 33.9 ns | 68.9 ns | 2.03 |
| Get | 10000 | 48 ns | 60.2 ns | 1.25 |
| Get | 100000 | 59.2 ns | 97.9 ns | 1.65 |
| Iterate | 1000 | 2.71e+04 ns | 7.96e+03 ns | 0.29 |
| Iterate | 10000 | 2.71e+05 ns | 8.13e+04 ns | 0.30 |
| IterateBinary | 1000 | 4.85e+04 ns | 6.29e+03 ns | 0.13 |
| IterateBinary | 10000 | 3.06e+05 ns | 5.9e+04 ns | 0.19 |
| LowerBound | 1000 | 213 ns | 135 ns | 0.63 |
| LowerBound | 10000 | 247 ns | 127 ns | 0.51 |
| LowerBound | 100000 | 296 ns | 162 ns | 0.55 |
| LowerBoundBinary | 1000 | 367 ns | 95.3 ns | 0.26 |
| LowerBoundBinary | 10000 | 385 ns | 176 ns | 0.46 |
| LowerBoundBinary | 100000 | 574 ns | 197 ns | 0.34 |
| MergeDisjoint | 1000 | 1.58e+04 ns | 1.08e+05 ns | 6.81 |
| MergeDisjoint | 10000 | 1.57e+05 ns | 2.09e+06 ns | 13.32 |
| MergeDisjoint | 100000 | 1.85e+06 ns | 2.82e+07 ns | 15.26 |
| MergeOverlapping | 1000 | 4.11e+04 ns | 1.07e+05 ns | 2.60 |
| MergeOverlapping | 10000 | 4.47e+05 ns | 2.11e+06 ns | 4.73 |
| MergeOverlapping | 100000 | 5.46e+06 ns | 2.88e+07 ns | 5.28 |
| MergeOverlappingBinary | 1000 | 5.67e+04 ns | 7.17e+04 ns | 1.26 |
| MergeOverlappingBinary | 10000 | 4.78e+05 ns | 1.4e+06 ns | 2.94 |
| MergeOverlappingBinary | 100000 | 1.46e+07 ns | 1.3e+07 ns | 0.90 |
| PersistentSet | 1000 | 4.94e+05 ns | 4.25e+05 ns | 0.86 |
| PersistentSet | 10000 | 6.71e+06 ns | 5.73e+06 ns | 0.85 |
| PersistentSet | 100000 | 9.24e+07 ns | 5.72e+07 ns | 0.62 |
| ReverseIterate | 1000 | 2.63e+04 ns | 8.07e+03 ns | 0.31 |
| ReverseIterate | 10000 | 2.64e+05 ns | 8.05e+04 ns | 0.30 |
| SplitBuildMerge | 1000 | 1.32e+05 ns | 2.35e+05 ns | 1.79 |
| SplitBuildMerge | 10000 | 1.48e+06 ns | 3.61e+06 ns | 2.43 |
| SplitBuildMerge | 100000 | 1.92e+07 ns | 4.99e+07 ns | 2.60 |
| SplitBuildMergeOverlapping | 1000 | 1.61e+05 ns | 2.55e+05 ns | 1.58 |
| SplitBuildMergeOverlapping | 10000 | 1.98e+06 ns | 3.99e+06 ns | 2.02 |
| SplitBuildMergeOverlapping | 100000 | 2.51e+07 ns | 5.39e+07 ns | 2.14 |
| SplitBuildMergePrefixed | 1000 | 2.16e+05 ns | 2.11e+05 ns | 0.98 |
| SplitBuildMergePrefixed | 10000 | 2.23e+06 ns | 2.53e+06 ns | 1.14 |
| SplitBuildMergePrefixed | 100000 | 2.69e+07 ns | 3.93e+07 ns | 1.46 |
| TransientGet | 1000 | 35.4 ns | 70.8 ns | 2.00 |
| TransientGet | 10000 | 47.7 ns | 61.9 ns | 1.30 |
| TransientGet | 100000 | 57.5 ns | 97.9 ns | 1.70 |
| TransientSet | 1000 | 1.15e+05 ns | 1.27e+05 ns | 1.10 |
| TransientSet | 10000 | 1.42e+06 ns | 2.42e+06 ns | 1.71 |
| TransientSet | 100000 | 2.41e+07 ns | 1.64e+07 ns | 0.68 |
| TransientSetPrefixed | 1000 | 2.13e+05 ns | 1.16e+05 ns | 0.54 |
| TransientSetPrefixed | 10000 | 2.31e+06 ns | 1.27e+06 ns | 0.55 |
| TransientSetPrefixed | 100000 | 2.77e+07 ns | 2.02e+07 ns | 0.73 |
| TransientUpdate | 1000 | 2.21e+05 ns | 5.88e+04 ns | 0.27 |
| TransientUpdate | 10000 | 2.46e+06 ns | 9.8e+05 ns | 0.40 |
| TransientUpdate | 100000 | 2.97e+07 ns | 1.07e+07 ns | 0.36 |
| UpperBound | 1000 | 238 ns | 150 ns | 0.63 |
| UpperBound | 10000 | 282 ns | 141 ns | 0.50 |
| UpperBound | 100000 | 321 ns | 186 ns | 0.58 |

Scans are where the design expected them: ordered iteration and
lower/upper bound are 2 to 8× faster. The one-version-per-key
`PersistentSet` is faster once the clone became a memcpy, `TransientUpdate`
and `TransientSetPrefixed` are 1.5 to 3× faster, and `TransientSet` is
mixed (slower at 1k and 10k keys, faster at 100k). Point lookup is the row
behind: 1.3 to 2× the radix tree's time in the tree alone. Vectorisation is confirmed and a binary search did
not help, so the cost is the fixed work per level (prefix compare, head,
tie compare) times the depth. It is a minor fraction of the engine's
728 ns `Get`, and the engine gate (G3) is the number that matters; the
tree-level row is recorded here so the follow-up has a baseline.

`merge` is 5 to 15× slower because it rebuilds instead of sharing
subtrees, as the design said it would; `SplitBuildMerge`, the recovery
shape, is 1.5 to 2.6× slower. The append builder and range-parallel merge
from the design are the follow-up, gated by the recovery rows of
`engine_bench`.

### Against the radix tree of PR #86

`main`'s radix tree carries per-child reference counts; #86 removes them
and is the fairer write baseline. Built from `radix-epoch-reclamation`
with the same `TransientInsertBatch` added (branch
`claude/pr86-map-bench-insert-batch`), run back to back with this branch's
binary on the same host, after the hints and the split-rule fixes.
`TransientInsertBatch` is the engine's write shape: a transient on an
existing tree, a lookup then a set for each of 100 new keys, publish.

| Benchmark | N | Radix main | Radix #86 | B+ tree | B+ / #86 |
|---|---:|---:|---:|---:|---:|
| Get | 1000 | 43.2 ns | 39.1 ns | 70.7 ns | 1.81 |
| Get | 10000 | 55.8 ns | 50.3 ns | 43.4 ns | 0.86 |
| Get | 100000 | 68.5 ns | 60.8 ns | 76.9 ns | 1.26 |
| Iterate | 1000 | 28.1 us | 16.2 us | 8.38e+03 ns | 0.52 |
| Iterate | 10000 | 278 us | 160 us | 84.2 us | 0.53 |
| LowerBound | 1000 | 211 ns | 110 ns | 130 ns | 1.18 |
| LowerBound | 10000 | 252 ns | 133 ns | 113 ns | 0.85 |
| LowerBound | 100000 | 307 ns | 149 ns | 140 ns | 0.94 |
| PersistentSet | 1000 | 489 us | 315 us | 428 us | 1.36 |
| PersistentSet | 10000 | 6.79e+03 us | 3.56e+03 us | 5.84e+03 us | 1.64 |
| PersistentSet | 100000 | 9.72e+04 us | 4.67e+04 us | 5.65e+04 us | 1.21 |
| SplitBuildMerge | 1000 | 129 us | 98.1 us | 239 us | 2.43 |
| SplitBuildMerge | 10000 | 1.59e+03 us | 1.18e+03 us | 3.86e+03 us | 3.27 |
| SplitBuildMerge | 100000 | 1.98e+04 us | 1.33e+04 us | 5.04e+04 us | 3.78 |
| TransientGet | 1000 | 41.1 ns | 38.6 ns | 68.8 ns | 1.78 |
| TransientGet | 10000 | 54.4 ns | 50.7 ns | 44.2 ns | 0.87 |
| TransientGet | 100000 | 65.9 ns | 60.8 ns | 76.9 ns | 1.26 |
| TransientInsertBatch | 1000 | 21.4 us | 14.2 us | 31 us | 2.18 |
| TransientInsertBatch | 10000 | 23 us | 16.4 us | 21.7 us | 1.33 |
| TransientInsertBatch | 100000 | 23.7 us | 17.9 us | 20.5 us | 1.15 |
| TransientSet | 1000 | 117 us | 74.5 us | 122 us | 1.64 |
| TransientSet | 10000 | 1.46e+03 us | 951 us | 2.33e+03 us | 2.45 |
| TransientSet | 100000 | 2.55e+04 us | 1.78e+04 us | 1.46e+04 us | 0.82 |
| TransientSetPrefixed | 1000 | 213 us | 124 us | 101 us | 0.81 |
| TransientSetPrefixed | 10000 | 2.28e+03 us | 1.4e+03 us | 1.28e+03 us | 0.91 |
| TransientSetPrefixed | 100000 | 2.73e+04 us | 1.57e+04 us | 1.93e+04 us | 1.23 |
| TransientUpdate | 1000 | 221 us | 148 us | 53.3 us | 0.36 |
| TransientUpdate | 10000 | 2.51e+03 us | 1.56e+03 us | 911 us | 0.59 |
| TransientUpdate | 100000 | 3.05e+04 us | 1.87e+04 us | 9.23e+03 us | 0.49 |

Against #86 the B+ tree is level or ahead on most of the tree at engine
scale and behind on two things. Behind: inserting new keys one version at
a time (1.2 to 1.6×) and the batched insert at small trees (2.2× at 1k
keys, 1.15× at 100k), both of which are the point lookup plus the slot
shift in a wide leaf; and `merge`, by design. Ahead: overwrites (0.4 to
0.6×), scans (0.5×), prefixed builds at small sizes, `LowerBound` at 10k
and 100k, and `Get` at 10k. `Get` at 100k is 1.26× and at 1k 1.8×; the
1k case is two levels and is not understood yet.

### Engine benchmarks, first integration

The engine builds and runs on either tree (`BYTECASK_KEYDIR=btree`), and the
full suite passes on both: 1494 cases, 11.4M assertions on the radix tree
and 8.7M on the B+ tree. Not one line of the engine needed a B+ tree
specific change; the tree's surface matched `EngineState`'s use as designed.

Two engine paths did need fixing, both ported from #86, because the B+ tree
enforces the version chain contract that the radix tree never has:
`EngineState::degraded_copy` marks a plain copy instead of deriving a
version from a state that may already have a successor, and `DB::resume`
drops the failed flush's heads before it derives. Without them 87 proof
cases failed with `a version that already has a successor cannot be derived
from again`. Both are no-ops on the radix tree.

`engine_bench`, 1M keys, 4 vCPUs, built without the RocksDB comparison
rows. Three interleaved rounds, radix then B+ tree inside each round so
host drift hits both sides, each round the median of 3 repetitions at 2s.
The spread column is the largest round-to-round ratio over the smallest,
and is what decides whether a row means anything.

| Operation | Radix | B+ tree | B+ / Radix | spread |
|---|---:|---:|---:|---:|
| Range-50 | 333 Kscans/s | 608 Kscans/s | 1.82 | 1.01x |
| Put (NoSync) | 172 Kops/s | 190 Kops/s | 1.10 | 1.01x |
| Put (Sync) | 7.3 Kops/s | 7.3 Kops/s | 1.02 | 1.20x |
| MixedBatch (Sync) | 305 Kops/s | 313 Kops/s | 0.99 | 1.19x |
| Get | 4.87 Mops/s | 4.51 Mops/s | 0.93 | 1.03x |

A ratio above 1 favours the B+ tree. The three rows with a 1 to 3% spread
are real: range scans 82% faster, no-sync puts 10% faster, point reads 7%
slower. The two sync rows have a 20% spread from fdatasync variance and
support nothing stronger than parity. `Del/Sync` is excluded entirely,
because its repetitions after the first delete keys that are already gone,
so it reports 1.9 Mops/s at 170% variance and measures nothing.

Against gate G3: `Range50` passes comfortably and `Put/NoSync` is better
than its −5% bound. `Put/Sync` and `MixedBatch` sit inside ±3% once
interleaved. `Get` is 7% slower and fails. That gap is far smaller than
the tree's own 26 to 80%, because the `pread` dominates a point read, so
only about a quarter of the tree's disadvantage reaches the engine.

Method matters here and cost a correction. The first pass ran every radix
row, then every B+ row. Its three low-spread rows came out the same, but
both sync rows drifted about 10 points in the B+ tree's favour and were
reported as wins when they are parity. Repeat variance within one run was
at or below 6% throughout, which measures only that a run is
self-consistent, not that two separately scheduled runs are comparable.

Concurrent reads (`GetMT`) cannot be measured on this host. The same
binary on the same row swings up to 2.2x between rounds, larger than any
difference between the trees, because the container has four shared vCPUs
and the benchmark spends most of its wall time populating before a two
second window. Three interleaved rounds put the B+ tree ahead at four
threads in all three (1.48x, 1.51x, 2.34x) and disagree at two threads
(0.72x, 1.16x, 1.37x). No number from it belongs in this document; it
needs the eight-core machine the README figures came from.

Recovery, same run, single iteration each but long enough to be signal:

| Threads | Radix | B+ tree | B+ / Radix |
|---|---:|---:|---:|
| 1 | 0.445 s | 0.425 s | 0.96 |
| 2 | 0.220 s | 0.532 s | 2.42 |
| 4 | 0.141 s | 0.809 s | 5.74 |
| 8 | 0.147 s | 1.260 s | 8.57 |
| 16 | 0.135 s | 2.290 s | 16.96 |

This is the predicted failure and it is worse than predicted. At one thread
there is no fan-in and the two trees are level. Every added thread creates
another partition and therefore another level of pairwise merges, and a B+
tree merge rebuilds rather than adopting subtrees, so the B+ tree gets
*slower* with more threads while the radix tree speeds up 3.3x. Gate G4
fails outright. Recovery cannot ship on the current rebuild-merge; it needs
the range-partitioned design in the recovery section above, where the fan-in
disappears entirely.

### Next steps

1. Recovery: the bulk loader plus the range-partitioned merge. This is now
   the blocking item, not a follow-up: gate G4 fails by 17x at 16 threads
   and no amount of tree tuning fixes it.
2. Point lookup, worth 10% of `Get` at the engine level.
3. `u32_map` on the B+ tree, `[model]` tests under ASAN and TSAN, sysbench
   against `main`.
3. Point lookup, which also sets the insert cost against #86: cut the
   per-level fixed cost, and try 2 KiB leaves for the short-key shapes
   where the scan and the slot shift are longest.
4. Remove the radix tree and update the documents (step 5).

## References

- Graefe, *Modern B-Tree Techniques* (2011): suffix truncation, prefix
  truncation, fence keys.
- Leis, Haubenschild, Neumann, *LeanStore* (ICDE 2018) and Neumann,
  Freitag, *Umbra* (CIDR 2020): heads and hints in slotted B-tree nodes.
- Okasaki, *Purely Functional Data Structures* (1998): path copying.
- Fraser, *Practical lock-freedom* (2004): epoch-based reclamation and its
  stalled-reader problem.
- `docs/radix_tree_epoch_reclamation_design.md` (#86 branch): the
  reclaimer this design reuses, and the review that shaped it.
