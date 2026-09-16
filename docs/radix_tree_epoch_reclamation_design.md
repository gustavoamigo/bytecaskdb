# Radix tree epoch reclamation — no reference counts on the write path

Status: implemented on `radix-epoch-reclamation`; see Implementation notes.
Intended as the implementation guide for a separate session; every code
reference is to `main` at `4538844`.

## Problem

Every write path-copies the radix tree: the nodes on the key's path are
cloned, the clones are linked into the new version, the old nodes are
released when the old version dies. Nodes are reference-counted
(`Node::refcount_`, `bytecaskdb/radix_tree.cppm:239`, through
`IntrusivePtr`, L87–164). The count is what makes a copy expensive: a node's
clone increments the refcount of every child it points to, and a release
decrements every child's, so a Node256 copy is 256 dependent touches on 256
other nodes, wherever they were allocated, and again on release. On a
5M-key tree those are cache misses.

Measured on MariaDB sysbench, 8 clients, 4 vCPUs, `/mnt/test-data`
(fdatasync ≈ 152 µs), September 15 2026:

- `perf` (DWARF call graph): `Node<KeyDirEntry>::release` 13–17% and
  `clone` 3–4% of the engine process's CPU, about 29 µs per insert. The
  callers are `execute_slots` (the leader dropping the previous head) and
  `commit_wait` (writers dropping their copy of the published state), so
  the cost sits inside the serialized stage 1 and on the writer's path.
- Upper bound of removing it, measured with a throwaway build in which
  `addref` and `release` are no-ops (nodes leak for the run):

| sysbench, 8 clients, 2 rounds | `main` (#78) | no refcounts | change |
|---|---|---|---|
| oltp_insert tps | 10,814 / 10,919 | 11,734 / 11,695 | +8% |
| oltp_insert avg / p95 | 0.74 / 1.12 ms | 0.68 / 1.00 ms | −8% / −11% |
| oltp_insert user CPU per tx | 116 µs | 78 µs | −38 µs |
| oltp_write_only tps | 3,964 / 3,885 | 4,695 / 4,658 | +19% |
| oltp_write_only avg / p95 | 2.02 / 3.55 ms | 1.70 / 2.78 ms | −16% / −22% |
| oltp_write_only user CPU per tx | 365 µs | 206 µs | −160 µs |

  System CPU rose 20–40 µs per transaction in that build only because leaked
  memory is never reused (every node is a fresh page fault); a real
  reclamation keeps the user-space saving without that. `engine_bench` on
  the root disk: `PutMT/Sync` +9% (2 threads), +8% (8), +40% (16),
  `Del/Sync` +17%, `Put/Sync` unchanged.

Two earlier experiments bound the alternatives: releasing old states on a
background thread was neutral (the work moved, the same 4 vCPUs paid it),
and the single-wait commit removed wakes but not this cost. The tree's
transient already mutates in place within a batch (edit tags), so the copies
that remain are one per touched path node per batch; the cost is per
child, and only a scheme that stops touching children removes it.

## Design in one paragraph

Nodes stop carrying a reference count. Sharing between versions stays
structural. A node is retired at the one moment the writer makes it
unreachable from the new version — when the transient clones it, or drops
the child pointer to it — onto a list owned by the transient. When the
transient freezes into a persistent tree, that tree gets a new epoch and the
list becomes that epoch's retired list. A persistent tree pins its epoch
while it lives (constructor, copy, destructor). A retired list of epoch `E`
is freed once no tree with an epoch below `E` is alive. Cloning a node is a
memcpy; releasing a version is a counter decrement; freeing is a batch of
`free()` off the write path.

## Current mechanism, what changes and what does not

| today | reference | after |
|---|---|---|
| `IntrusivePtr<Node>`: `addref` on copy, `release` on destruction | L87–164, L281–283, L292–368 | `NodePtr`: a plain pointer wrapper, no counting |
| `Node::refcount_` (atomic, starts at 1) | L239 | removed; `packed_tag_` keeps node type, has-value and the edit tag |
| `release()`: iterative cascade freeing a node and its exclusively-owned children | L292–368 | removed; freeing is by retired list |
| `clone()`: allocates the same node type and copies children through `IntrusivePtr` (per-child addref) | L970–1016, per-type helpers L690–728 | memcpy of the child array |
| `ensure_mutable`: in place if `edit_tag == tag && refcount == 1`, else `clone_for(tag)` | L1859–1870 | in place if `edit_tag == tag`; else clone and **retire the source** |
| edit tags (`next_edit_tag`, `set_edit_tag`) | L186, L381–386 | unchanged |
| `insert_child` promoting a node to a wider type (`make_node16_like` …) discards the narrower node | L738–854 | narrower node is retired if foreign, freed at once if owned by this transient |
| `remove_child`, `merge_with_child_transient` (path compression) discard a node | L866–967, L2071–2099 | same rule |
| `erase_transient` removing a child | L2015–2069 | the removed subtree is walked and retired |
| `TransientRadixTree::persistent()` | L1813–1818 | also stamps the epoch and hands the retired list to the registry |
| `PersistentRadixTree` (root, size) | L1176–1279 | (root, size, epoch); ctor/copy/dtor pin and unpin the epoch |
| `PersistentRadixTree::set` / `erase` (persistent one-shot updates) | L1216–1231, L1424–1564 | implemented as a one-node transient (`transient().set().persistent()`) so they follow the same rules |
| `PersistentRadixTree::merge` (recovery fan-in) | L1605–1730, used at `bytecask.cppm:3620` | retires the nodes of both inputs it does not reuse |
| iterators hold `IntrusivePtr` frames | L2195–2243, L2577–2607 | hold a copy of the tree handle (an epoch pin) plus raw frames |
| `PersistentU32Map` (`files`, `file_stats`) | `u32_map.cppm` | untouched; it does not use `IntrusivePtr` |
| `EngineState`, `Snapshot`, `EntryIterator`, `KeyIterator`, `TlState` hold `shared_ptr<const EngineState>` | `bytecask.cppm:1146`, 291, 225, 3258 | unchanged; the state's `key_dir` member is the epoch pin |

## The garbage rule

A node becomes garbage at exactly one of these moments, all on the thread
that runs the transient:

1. **Superseded.** `ensure_mutable` clones a node whose edit tag is not the
   transient's. The source is retired. (A node whose tag is the transient's
   is private to it and is mutated in place; no garbage.)
2. **Replaced by promotion or compression.** `insert_child` grows a node to
   the next type, `remove_child` shrinks it, `merge_with_child_transient`
   collapses a routing node into its only child. The discarded node is
   retired if foreign, freed immediately if it carries the transient's tag
   (nothing outside the transient can reach it).
3. **Dropped.** `erase_transient` unlinks a child; the transient walks the
   unlinked subtree and retires every node in it (a leaf is one node; a
   `del_range` can be a large subtree — the walk is the cost of the range
   delete, today paid by the release cascade).
4. **Never published.** A transient destroyed without `persistent()` (the
   engine's error paths: `err_t` transients that degrade, a batch that fails
   after building) frees every node it created and discards its retired
   list, since the base version is still reachable and nothing it built is.

The transient therefore keeps three lists: `created_` (nodes it allocated,
freed on discard, forgotten on `persistent()`), `dead_owned_` (rule 2 for
owned nodes, freed on either exit), `retired_` (rules 1–3 for foreign nodes,
handed to the registry on `persistent()`, dropped on discard).

Invariant to test: `allocated − freed − retired_pending == reachable nodes`
over the whole test suite, with a test-only global accounting counter.

## Epochs and the registry

```
struct EpochRegistry {                       // one per process, in radix_tree.cppm
  std::mutex mu;
  std::uint64_t next_epoch{1};
  std::map<std::uint64_t, std::uint32_t> live;        // epoch -> live tree handles
  std::deque<std::pair<std::uint64_t, std::vector<Node*>>> retired;  // by epoch, ascending
};
```

- `persistent()`: `epoch = next_epoch++`; if the transient's `retired_` is
  non-empty, append `(epoch, retired_)`; register the new tree.
- Tree handle constructor/copy: `live[epoch]++`. Destructor: `live[epoch]--`,
  erase at zero, then reclaim.
- Reclaim: with `min_live` = the smallest key in `live` (or infinity), free
  every retired entry whose epoch `<= min_live`. Correctness: a node retired
  while building epoch `E` is reachable only from trees with epoch `< E`;
  when the smallest live epoch is `>= E` no such tree exists.
- The frees are done after releasing `mu` (swap the freeable entries out
  under the lock, free outside), on the thread that dropped the last handle
  of the oldest epoch: usually the flusher dropping the previous published
  state, sometimes a reader dropping a snapshot. Bounded: one epoch's worth
  of retired nodes, plain `free`, no tree walk. If it shows up on the flush
  path it can be handed to the background worker, but measure first.
- The transient must not outlive the persistent tree it was derived from
  (its base nodes are pinned by that tree's epoch). True today for every
  transient in the engine (`execute_slots` keeps `current` alive across the
  batch); state it as a contract of `TransientRadixTree` and assert it in
  debug builds by having the transient hold a copy of the base handle.
- A `del_range` or a large erase retires a subtree of any size; the retired
  list is a vector of pointers, so memory for the list itself is one pointer
  per node until reclaim.

Retention compared to today: a long-lived snapshot pins the nodes it can
reach under either scheme. The difference is granularity — with epochs a
node retired at `E` waits for every tree below `E`, not only for the
snapshots that reach it — and for the engine's linear version history the
two are the same set. Recovery and vacuum build trees the same way and get
the same rule.

## Recovery and merge

Recovery builds one tree per hint-file partition and merges them pairwise
(`docs/parallel_recovery_design.md`; `bytecask.cppm:3620`). `merge_impl`
(L1605–1730) creates nodes for split prefixes and reuses subtrees of both
inputs. Under epochs the merge result is a new tree with its own epoch; the
nodes of `a` and `b` that the result does not reuse are retired onto the
result's list, and the input handles pin them until the recovery code drops
them. This is the same rule as the transient, applied to two bases instead of
one. The parallel builders are transients that end in `persistent()`, so no
special case.

## Iterators

`RadixTreeIterator`, `ValueIterator` and their reverse forms keep a stack of
frames that today hold `IntrusivePtr`s, which keeps the nodes alive even if
the tree handle is gone. After the change an iterator must not outlive its
tree. Make that mechanical: each iterator holds a copy of the
`PersistentRadixTree` handle it was created from (one epoch pin, O(1)) and
raw node pointers in its frames. The engine's iterators already hold the
`EngineState`, so nothing changes at that level; the tree unit tests that
iterate a temporary tree need the same guarantee and get it from the handle
copy.

## Concurrency

- Retired and created lists belong to one transient and are touched by one
  thread. The registry's mutex is taken once per `persistent()` and once per
  tree handle construction and destruction; per batch that is a handful of
  uncontended lock operations, against 256 cache misses per wide node today.
- Readers hold a tree handle (through their `EngineState`) and never touch
  reclamation. A node is freed only after every handle that could reach it
  is gone, so no read can observe a freed node.
- `ensure_mutable`'s `refcount == 1` term goes away. The edit tag alone is
  sufficient: a node stamped with the current transient's tag was created by
  it and is reachable only through it until `persistent()`.
- TSAN and ASAN over the full suite are the proof; ASAN's leak detection is
  the accounting check for the process as a whole, the test-only counter
  the check per test.

## What this does not change

The tree's shape, key encoding, node types and promotion thresholds; the
transient's edit-tag semantics within a batch; `EngineState` and snapshot
semantics; the two-heads commit pipeline; the on-disk format; `u32_map`.
Snapshots stay O(1) (a handle copy is a pointer and a counter increment).

## Tests

Tree unit tests (`tests/radix_tree_test.cpp`), each with the accounting
invariant checked at the end:

- clone by supersede: two versions, drop the old one, its unique nodes are
  freed only after the drop; drop order reversed as well.
- promotion and compression: inserts that grow Node4→16→48→256 and erases
  that shrink and collapse, owned and foreign nodes both.
- erase of a subtree and range erase: every node under the removed child is
  freed after the old version dies.
- snapshot held across N transients: nothing reclaimed until it is
  released, then every retired epoch drains in one reclaim.
- discard without `persistent()`: created nodes freed, base untouched and
  still readable.
- merge of two trees, then drop the inputs: unreused nodes freed, result
  intact.
- iterator over a version while newer versions supersede its whole path:
  the iterator completes and sees its version.
- many threads reading snapshots while one writer runs batches (existing
  `reader_ok` pattern at `tests/radix_tree_test.cpp:714`), under TSAN.

Engine tests: the `[model]` recovery tests unchanged; the `[pipeline]`
stress test unchanged; the memory profile benchmark
(`benchmarks/memory_profile.cpp`, `scripts/run_memory_profile.py`) before
and after, per phase.

## Validation gate

- H1: `Node::clone` + `Node::release` (or their successors) below 5% of
  engine CPU under sysbench oltp_insert (baseline 16–21%).
- H2: sysbench 8 clients, two interleaved rounds against the same base:
  oltp_insert ≥ +6% tps, oltp_write_only ≥ +12% (upper bound measured +8%
  and +19%).
- H3: `engine_bench` `Get`, `Range-50`, `PutMT/Sync` 2–64 threads and
  recovery (1M and 10M keys) within ±3% or better; steady-state RSS in the
  memory profile within 5% of today.
- H4: full suite clean under ASAN with leak detection, and under TSAN.
- H5: the accounting invariant holds in every tree unit test.
- H6: all existing tests pass unchanged.

## Implementation plan

1. Test-only allocation accounting in the tree (`allocated`, `freed`,
   `retired_pending`) and a reachable-node walk; add the tree unit tests
   above against today's refcounted tree so they pass before the change.
2. `EpochRegistry`; `PersistentRadixTree` gains `epoch` and pins it in
   ctor/copy/dtor; `persistent()` stamps and hands over an (empty) list.
   Refcounts still in place; nothing freed by epochs yet. Suite green.
3. Transient lists (`created_`, `dead_owned_`, `retired_`) and the four
   garbage-rule sites; still with refcounts, asserting in debug builds that
   every node the refcount frees was on a list (or owned). This is the step
   that finds missed sites while refcounts still protect memory.
4. Replace `IntrusivePtr` with `NodePtr`, remove `refcount_`, `addref`,
   `release`; clone becomes memcpy; reclaim frees. Iterators take a handle
   copy. `merge` retires unreused input nodes. Persistent `set`/`erase` go
   through a transient.
5. Sanitizers, memory profile, benchmarks; record H1–H6 in this document.
6. Update `docs/persistent_radix_tree_design.md` §3.1 (node layout) and §3.2
   (transient model: retire lists, epochs), the memory section of
   `docs/bytecask_design.md`, and the README's architecture paragraph if a
   user-visible characteristic changes (none expected).

---

## Implementation notes

Implemented on `radix-epoch-reclamation`. The shape is as designed —
no reference counts, retirement at the four garbage sites, a registry that
frees off the write path — but three things differ from the sketch above,
each because the sketch was wrong about something. They are recorded here
because the reasoning is the load-bearing part.

### Reclamation is per node, not per epoch

The design frees a retired list of epoch `E` once no tree with an epoch
below `E` is alive. Built that way, one long-lived `db.snapshot()` pins
every node retired after it, so memory grows with the write rate for as
long as the snapshot is held — the stalled-thread problem that epoch-based
reclamation is known for. Reference counting does not do this: a snapshot
holds what it can reach and nothing else. The memory test for structural
sharing caught it, at 378 KB where 220 KB was expected.

The fix is to ask the question per node instead of per list. Tags increase
with every session and a session starts only after its base was published,
so a node created by session `S` is reachable only from the version `S`
published and from versions derived from it, all with tags at or above `S`.
A node retired by session `R` is by definition not reachable from `R` or
anything below it. So a live version `V` reaches it exactly when `tag(V)`
falls in `[tag(X), tag(R))`. Each retired node is parked on the live version
that currently blocks it — the smallest live tag at or above its own — and
freed the moment no live tag falls in its window. A version's death
re-examines only what was parked on it. Retention is then the same set
reference counting held, and the cost is a `lower_bound` over the live
versions (one to three of them) per retired node.

### Versions form a graph, and a dead one is not always removable

A version whose last handle goes while nothing was derived from it has to
free what only it could reach, and the test is the same tag comparison:
above every version it was derived from, and nothing else can reach it.
That requires knowing its live ancestors, so the registry keeps the
derivation graph and splices a dead version out, its successor inheriting
its bases — which is what lowers the successor's floor onto the nodes it now
holds alone, and why the last version of a lineage frees the whole
structure.

Splicing unconditionally is wrong. The engine forks: a failed flush leaves
an unpublished head, and the `resume()` that clears it derives a second
version from the same published state. When that shared base died, both
branches inherited its ancestors, the floor dropped below the base's own
nodes, and the first branch to die freed nodes the other was still reading —
found by ASAN as a heap-use-after-free in `contains_key` after `resume()`.
A dead version with two or more successors is therefore kept as a tombstone:
they share its nodes with each other, and it is what stops either one's walk
from freeing what the other reads. It is spliced out when their number falls
back to one.

The same fork breaks the window argument for parking, since a fork can reach
what its sibling superseded while carrying a larger tag, so parking is
suspended while any base has more than one successor and resumes when that
resolves. The engine is forked only between a failed flush and its `resume()`.

### The tag word has to be atomic

Two versions derived from one base can both supersede the same node, so
retirement is marked in the node (bit 59) and a node already marked is left
alone. That bit is the only write a node receives after its session
published, and readers of other versions read the same word — TSAN reported
77 races on it. The word is now `std::atomic<std::uint64_t>` with relaxed
access everywhere: same node size, and on every target this builds for the
loads and stores are the plain instructions they were.

### The registry has to be allocation-free per version

The first working version published each tree version into a `std::map` of
records and a `std::set` of live tags, with the retired nodes grouped into
fresh vectors and filed in another map. That is five or six heap allocations
per published version, and `map_bench` showed it plainly: the batched paths
were 15–20% faster as intended, but `PersistentSet` — one published version
per key, which is what the engine's single-writer no-sync path looks like —
was 87% slower at 1k keys. `perf` put `malloc`/`free` at 34% of that
benchmark, with red-black tree rebalancing behind it.

There are only ever a handful of live versions, so the registry now keeps
them in flat vectors searched linearly: records sorted by tag, and the
derivation graph as a flat list of base→successor pairs. Retired nodes stay
in the vector the session already built and are moved, not regrouped,
because the nodes of one parcel almost always share a blocker. Parcels hang
off the record itself rather than a second map, and the scratch used while
collapsing the graph is reused under the lock. Publishing a version then
allocates nothing once the buffers have settled, and they are handed back
when the last tree of that value type goes. `PersistentSet` went from −87% /
−53% / −33% to −16% / +4% / +12% at 1k / 10k / 100k.

A transient that was never written to is also no longer published at all: it
hands back its base's own handle. The engine opens a transient per batch on
three maps, and the file registry changes only on rotation.

### Validation

| Gate | Result |
|---|---|
| H1 | Not measured directly. `perf` on this host can attribute the tree's own symbols only in `map_bench`; in `engine_bench` the binary is stripped of the symbols needed. |
| H2 | oltp_insert +6.5% to +8.9%, meeting the ≥ +6% gate. oltp_write_only +6.7% to +10.3%, below the ≥ +12% gate. See below. |
| H3 | `Get` +5%, `Range50` +18%, recovery +2% to +14%. `PutMT/Sync` is 4–9% *down*, outside the ±3% band. |
| H4 | Full suite clean under ASAN with `detect_leaks=1`, and under TSAN. |
| H5 | Node accounting asserted in eight new tree tests (`[accounting]`). |
| H6 | Full suite passes: 1485 cases. `radix_tree_memory_tests` passes with net bytes 0. |

### Measurements

Same host as the problem statement: 4 vCPUs, `/mnt/test-data`. Each pair of
rounds is interleaved branch/main so that host drift hits both sides.

sysbench, 8 clients, 1M rows, 20 s, two rounds:

| | main r1 / r2 | branch r1 / r2 | best-of | mean |
|---|---|---|---|---|
| oltp_insert tps | 14,802 / 15,708 | 16,726 / 16,505 | +6.5% | +8.9% |
| oltp_write_only tps | 5,656 / 6,085 | 6,451 / 6,495 | +6.7% | +10.3% |
| oltp_insert avg / p95 ms | 0.54 / 0.81, 0.51 / 0.72 | 0.48 / 0.68, 0.48 / 0.70 | −6% / −3% | |
| oltp_write_only avg / p95 ms | 1.41 / 2.39, 1.31 / 2.07 | 1.24 / 2.00, 1.23 / 2.03 | −6% / −2% | |

So oltp_insert reproduces the no-refcount upper bound of +8%; oltp_write_only
reaches about half of its +19%.

`engine_bench`, 1M keys, best of two interleaved rounds:

| benchmark | change |
|---|---|
| Get | +5.0% |
| Range50 | +17.7% |
| Recovery 1 / 4 / 16 threads | +14.3% / +2.9% / +1.7% |
| Del/Sync | +1.3% |
| MixedBatch/Sync | −0.7% |
| Put/Sync | −5.7% |
| Put/NoSync | −5.9% |
| PutMT/Sync 2–64 threads | −4.8%, −9.4%, −3.9%, −3.8%, +1.6%, −0.7% |

`map_bench`, best of two interleaved rounds: TransientSet +17% to +22%,
TransientSetPrefixed +13% to +15%, TransientUpdate +13% to +14%,
MergeDisjoint +21% to +26%, SplitBuildMergePrefixed +13% to +16%.
`MergeOverlappingBinary/100000` is −26%, a wide-node shape the engine's
recovery merge does not produce; it was not chased further.

### What is left

The tree itself is 13–22% faster on every batched path, and the engine gains
on reads, range scans and recovery, but the engine's write path is still
4–9% behind on `engine_bench` and oltp_write_only reaches only half its
upper bound. `perf` on `engine_bench`'s no-sync put attributes 4% of the
branch's time to `pthread_mutex_lock`/`unlock`, against none on `main`, plus
about 5 points more in `malloc`/`free`.

The cause is structural rather than incidental: a commit opens a transient on
three persistent maps (key directory, file registry, file stats), and each
one pins its base, publishes, and drops the old version through one
process-wide mutex per value type. `main` paid nothing here because a handle
copy was a per-node atomic increment with no shared lock.

The fix that follows from this is to make pinning lock-free: give records
stable addresses and an atomic handle count, have the tree handle hold the
record pointer rather than the tag, and take the lock only when a count
reaches zero or a version is published. That removes the lock from every
handle copy and from every drop but the last, leaving two lock acquisitions
per version instead of four to six. It is not done here.
