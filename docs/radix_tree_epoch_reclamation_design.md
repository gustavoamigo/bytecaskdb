# VersionChain — version-chain node reclamation for persistent trees

Status: implemented (`bytecaskdb/version_chain.cppm`). Used by the keyed
B+ tree (`bytecaskdb/btree.cppm`, through `btree_detail::ChainTraits`) and
the blind-leaf B+ tree (`bytecaskdb/blind_btree.cppm`), which reuses the
keyed tree's inner nodes and chain.

It was first designed for and built in the radix tree (PR #86), since
retired; the file keeps its old name because other documents link to it.

## Problem

The key directory is a persistent tree. Every write path-copies it: the
nodes on the key's path are cloned, the clones are linked into a new
version, and every node off the path is shared between the old version and
the new one. Readers hold old versions (the published state, snapshots,
iterators, per-thread read caches) for as long as they like.

So when can a node be freed? The obvious answer is a reference count per
node. Its cost is per child: cloning an inner node increments the count of
every child it points to, and freeing it decrements every child's, each one
a likely cache miss on a large tree. On the radix tree, under MariaDB
sysbench, the release and clone paths were 16–21% of the engine's CPU, and
a build with counting disabled (leaking every node) was 8% faster on
`oltp_insert` and 19% faster on `oltp_write_only`. That cost sat on the
serialized commit path.

`VersionChain` frees nodes with no per-node count. Cloning a node copies its
child pointers and touches nothing else; dropping a version is one counter
decrement; freeing is a batch of plain deletes.

## What the tree supplies

The chain is generic over a `Traits` type:

```cpp
using Node = ...;
static auto tag(const Node *) -> std::uint64_t;            // creating session
template <typename F> static void for_each_child(Node *, F &&);
static void destroy(Node *) noexcept;                      // free one node
static void account_retired(std::int64_t) noexcept;        // test accounting
```

The tree keeps one invariant the chain relies on: a node's children are
never newer than the node. A session links new children only into nodes it
owns.

## Versions and sessions

A *session* builds one new version from a base. It takes a fresh tag from
`new_version_tag()`, a process-wide counter, and stamps it into every node it
creates. A node carrying the session's tag is private to it and is edited in
place. A node with another tag is foreign: the session copies it and
*retires* the original — puts it on its retired list. In the B+ tree all of
this happens in `BuildSession::discard`; a session dropped without publishing
frees what it created (a walk from its root that stops at the first node
with a different tag) and forgets its retired list.

On publish the session hands its tag, its base's tag and its retired list
to the chain. The published version is identified by the session's tag.

A tree handle (`PersistentBTree`, `PersistentBlindBTree`) is
`(root, size, version)`. Copying it calls `pin(version)`; destroying it
calls `unpin(version, root)`. That is the only lifetime work a handle does.

## The chain contract

A version may be derived only from a version that has no successor.
`publish` enforces it: if the base already has a successor it throws
`std::logic_error`, changes nothing, and leaves the retired list with the
session.

Versions derived from one another form a *lineage*. A version derived from
the empty tree (base 0) starts a new one, which is how bulk loads and
recovery builds publish.

With the contract, tags order the versions of a lineage, and reachability
becomes an interval. Tags increase with every session and a session starts
only after its base was published, so:

- a node created by session `S` is reachable only from `S`'s version and the
  versions derived from it;
- a node retired by session `R` is reachable from none of `R`'s version or
  its descendants.

A node created by `S` and retired by `R` is therefore reachable from exactly
the versions of its lineage with tags in `[S, R)`.

The contract replaced an earlier design on the #86 branch that allowed
several versions to be derived from one base and tracked the derivation
graph. Review showed it could free a node a live sibling still reached. The
engine derived from a non-head in one place, `DB::resume`, which now resets
its head to the published state before deriving.

## The free rule

`blocker_for` is the only free rule. For a retired node, the blocker is the
smallest live tag of its lineage in `[S, R)`. If there is none, the node is
freed. Otherwise it is *parked* on that version.

When a version with a successor loses its last handle, only the nodes
parked on it are re-examined: each moves to the next live version in its
window, or is freed. There is no scan of everything retired.

Retention is per node, not per epoch. A first version on the radix tree
freed a whole retired list once no older version was alive; one long-lived
snapshot then pinned every node retired after it, and memory grew with the
write rate for as long as it was held. Under the interval rule a version
holds exactly the nodes it can still reach — the same set reference
counting would hold. Under random writes a version held long enough still
ends up holding the whole tree it was taken from.

## Retraction

When a version with *no* successor loses its last handle, the lineage
shrinks back to its newest live predecessor `F`, or ends if there is none.
The versions above `F` are a dead segment nothing reaches:

- nodes reachable from the dead root with a tag above `F` were created by
  the segment and are freed by a walk, which stops at the first node at or
  below `F` because children are never newer than their parent;
- nodes the segment retired (retiring tag above `F`) are reachable from `F`
  again and are unparked. With no `F` the lineage is over and they are freed.

`F` is then the head again and may be derived from. This one rule covers a
head dropped after a failed flush (`DB::resume`), a version a test publishes
and drops, the last handle at DB close, and the tail of a recovery build.

## Threading and latency

- All chain state is under one mutex. `publish`, `pin` and `unpin` each take
  it once.
- Reclamation runs on the thread that drops the last handle: usually the
  writer replacing the published head, sometimes a reader dropping a
  snapshot. Freeing retired nodes happens after the mutex is released.
- The retraction walk is the exception and holds the mutex: once a version
  leaves the chain, another thread could decide that what it reached is free
  while the walk is still stepping through it. A retraction after a large
  build therefore holds the lock for a walk proportional to what the dead
  segment created.
- Readers never touch the chain beyond pinning. A node is freed only after
  every version that can reach it is gone.
- Publishing allocates nothing in the steady state. There are only ever a
  handful of live versions, so records live in a flat vector sorted by tag
  and searched linearly. A retired list is moved into the chain as one
  *parcel*, not regrouped, because the nodes of one parcel almost always
  share a blocker; the first parcel on a version is stored inline and only
  further ones spill into a vector. On the radix tree, the first
  implementation (a `std::map` of records and a `std::set` of live tags) made
  one-version-per-key inserts 87% slower at 1k keys, with `malloc`/`free` at
  34% of the profile; the flat layout brought that to −16%.
- When the last version of a value type goes, the chain shrinks its buffers,
  so an idle process holds nothing on its behalf.
- The chain itself is immortal: constructed once in static storage and never
  destroyed, since a tree can outlive static destruction.

Known cost: the mutex. On the radix tree, `perf` attributed about 4% of a
no-sync put to `pthread_mutex_lock`/`unlock`. Issue #85 proposed lock-free
pinning (stable record addresses, an atomic handle count, the lock taken only
when a count reaches zero or a version is published); it has not been done.

## Accounting and observability

- `gauges()` returns the number of live versions and parked nodes. The engine
  reports them in `stats()` as `bytecask.keydir_versions_live` and
  `bytecask.keydir_nodes_parked`. More than one version means a snapshot,
  iterator or read cache is holding an older tree; parked nodes climbing
  towards the tree's node count while versions stay above one names a stale
  holder, not a leak.
- `parked_nodes()` (test-only) lists every parked node.
- `Traits::account_retired` keeps a running count of retired-but-not-freed
  nodes. The B+ tree tests (`tests/btree_test.cpp`,
  `tests/blind_btree_test.cpp`) check, after each scenario, that allocated
  minus freed equals the nodes reachable from live handles plus the parked
  ones; the keyed tree's tests also check that the retired count equals the
  parked count.
