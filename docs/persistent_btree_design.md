# Persistent B+ tree key directory — design

Status: **proposal, design phase**. Nothing in this document is implemented.
Date: 2026-09-17
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
| D3 | Node size | `kNodeBytes = 4096`, compile-time. A node is larger only when a single key needs it, and such a node holds exactly one entry. | 3 levels at 1M keys and 4 at 100M for 36-byte keys. Reads are the priority (§Performance). 2 KiB is the one alternative to benchmark (§Plan step 4). |
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

**Point lookup.** Three dependent node visits at 1M keys, four at 100M,
against five to twelve for the radix tree on text keys (one per prefix
chunk of 7 bytes plus one per branch). Each visit is a prefix compare, one
pass over the slot array and usually one suffix compare: about three
cache lines per level. Expect `Get` equal or better; the tree is a minor
part of the 728 ns p50, most of which is the `pread`.

**Single-key commit.** The path copy is three or four packed nodes of
about 2.5 KiB each: 8 to 10 KiB of copy and four 4 KiB allocations,
against a few 40 to 176 byte nodes today. About 0.3 µs more per commit.
`Put/Sync` (7 ms per op) does not see it; `Put/NoSync` (7.5 µs per op) is
expected 3 to 5% slower. This is the one row expected to regress and the
gate allows it up to 5%.

**Batched writes.** After the first touch a node is owned and edited in
place with a slot shift and an entry write. A batch of random keys copies
one leaf per distinct leaf touched, about 2.5 KiB each; a batch of nearby
keys copies almost nothing. Expect `MixedBatch`, `PutMT/Sync` and sysbench
within noise of #86, and `TransientSet` in `map_bench` faster: no node
promotion, no chain splitting.

**Range scans.** Keys inside a leaf are adjacent; one leaf hop per 60
keys. Expect `Range50` and `Iterate` well ahead. `LowerBound` ahead by the
same argument as `Get`.

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

If G3's `Put/NoSync` or G2 fails, the 2 KiB node is tried before anything
else is changed; it halves the copy per level and adds one level at 100M.

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

1. **Node size.** 4 KiB is proposed for read depth; 2 KiB is the fallback
   if the single-key write cost or memory misses its gate. Accept 4 KiB as
   the default with the comparison in step 4?
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
