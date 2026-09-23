# Blind-leaf B+ tree key directory — design

> **Status: proposal.** Not a committed plan. It describes a third key
> directory beside the B+ tree and the radix tree, for deployments where the
> number of keys, not the value data, is what runs out of RAM. Nothing here
> has been measured; every number is an estimate that §Gates turns into a
> measurement before the engine depends on it.

Baseline for code references: `main` at the time of writing
(`bytecaskdb/btree.cppm`, `bytecaskdb/bytecask.cppm`,
`bytecaskdb/internals.cppm`).

## Problem

The key directory holds every key in full. The B+ tree
(`docs/persistent_btree_design.md`, §Memory) measures 33–42 B/key on
structured keys and 71–133 B/key on random ones, and the floor is structural:
a 16-byte `KeyDirEntry`, a 2-byte length, the key suffix rounded to 8, and an
8-byte slot come to 32 bytes per key before fill. Random keys fill leaves to
about 70%, and long random keys (UUIDv4 text, SHA-256) carry 30–60 suffix
bytes each. At 128 GB that is between one and four billion keys, depending on
the key shape.

The keys are already on disk twice: in the data file entry and in the hint
file. The key directory needs them only to order entries and to confirm a
match, and a B+ tree can do both without storing them.

## Idea

In a sorted set of `n` keys, only the `n - 1` bit positions where adjacent
keys first differ (their *crit bits*) are needed to find where a key belongs.
A leaf that stores those positions instead of key bytes can do a **blind
search**: it tests only the crit bits of the search key and arrives at one
*candidate* entry. If the key is in the leaf, the candidate is that key. If it
is not, the candidate is some neighbour, and one read of the candidate's key
from its data entry says so and gives the exact position.

This is the Patricia trie's search, done over a sorted array instead of a
tree of nodes. The String B-tree (Ferragina and Grossi, 1999) puts a blind
trie in every node; HOT (Binna et al., SIGMOD 2018) is a height-balanced tree
of blind nodes. This design makes only the **leaves** blind:

- **Inner nodes stay as they are.** Suffix-truncated separators, the node
  prefix, the head search: routing to the correct leaf is exact and needs no
  I/O. Inner nodes are about 1/60 of the nodes, so their key bytes cost
  under 1 B/key.
- **Leaves drop their key bytes.** An entry is a data file location, the
  entry's on-disk size, a crit bit and a fingerprint: 16 bytes, no slot, no
  heap.
- **Everything around the tree stays.** Path copying, `BuildSession`,
  `VersionChain` and its three traits, the iterator stack and the bulk
  loader keep their contracts. Only the leaf layout and the algorithms that
  touch it change.

## Goals and non-goals

Goals:

- 16–25 B/key for any key shape and length, against 33–133 B/key today.
- A point read costs the same one read it costs today. A lookup of an absent
  key usually costs none.
- Every contract in `CONTRACT.md` holds unchanged, with one addition: a
  write can now fail because of an I/O error on a *neighbouring* key's entry
  (§Failure modes).
- Same persistence model: O(1) snapshots, lock-free readers, one writer.

Non-goals:

- Replacing the B+ tree as the default. This is a mode for key-count-bound
  deployments, chosen at build time like `BYTECASK_KEYDIR=radix` today
  (§Selection).
- Keeping `keys_from` free of I/O. It cannot be: the tree has no key bytes to
  return.
- Keeping the key directory's size proportional to anything but the key
  count. That is the point.

## Key encoding and crit bits

Keys are byte strings of any length up to 65,535, and one can be a prefix of
another (`"ab"`, `"ab\0"`, `"abc"`). Zero-padding a short key would make
`"ab"` and `"ab\0"` identical, so crit bits are defined over an encoding that
keeps byte-lexicographic order and gives every key a distinct bit string:

```
each key byte b  →  1 b7 b6 b5 b4 b3 b2 b1 b0      (9 bits: continuation, then the byte)
end of key       →  0                              (then 0 forever)
```

Bit `p` of key `k` is:

```cpp
// 9 bits per byte; p / 9 is the byte, p % 9 == 0 is its continuation bit.
auto bit(std::span<const std::byte> k, std::uint32_t p) -> bool {
  const auto i = p / 9;
  const auto r = p % 9;
  if (i >= k.size()) return false;                  // past the end: 0
  if (r == 0) return true;                          // a byte is present
  return (std::to_integer<unsigned>(k[i]) >> (8 - r)) & 1u;
}
```

A shorter key has `0` where a longer key with the same prefix has its
continuation `1`, so it sorts first, which is what `std::ranges::lexicographical_compare`
does on bytes. `crit(a, b)` is the first `p` where `bit(a, p) != bit(b, p)`.
The largest crit bit for a 65,535-byte key is 589,815, so 20 bits hold it.

The byte-level common prefix used for separators is `crit(a, b) / 9`.

## Leaf layout

```
  ┌──────────────┬────────────────────────────┬──────────────────────────────────┐
  │ header 32 B  │ meta: u32 × capacity       │ entry: u64 loc, u32 size × cap   │
  └──────────────┴────────────────────────────┴──────────────────────────────────┘

  meta  = crit:20 | fp_lo:12        crit of this key against the previous key in the leaf;
                                    unused for index 0
  loc   = file_id:20 | offset:32 | fp_hi:12
  size  = entry_bytes:29 | 0:3      header + key + value + CRC of the data entry
```

The header is the same 32-byte `NodeHeader` inner nodes use (tag, capacity,
count, `is_leaf`); `prefix_len`, `heap_floor` and `dead_bytes` are zero in a
blind leaf. The arrays are structure-of-arrays so the search reads only
`meta`: 4 bytes per key, 256 bytes for a 64-entry leaf.

The 24-bit fingerprint (`fp_lo`, `fp_hi`) is a hash of the full key, taken
when the key is inserted and never recomputed. It is not used for ordering.
It lets a lookup reject a candidate that is not the key without reading
anything, with a false-match rate of 1 in 16.7 million.

`entry_bytes` replaces `value_size`. `get` reads exactly that many bytes, and
live-bytes accounting needs exactly that number (`entry_size(key, value)`)
without knowing the key length. The largest entry, a 256 MiB value and a
65,535-byte key, fits in 29 bits.

What leaves the key directory: the 48-bit `sequence` and the key bytes.
§Sequence without a sequence field shows where each use of `sequence()` goes.

Capacity is a compile-time constant, `kBlindLeafEntries = 64`: 32 + 64 × 16
= 1,056 bytes. Blind search is linear in the leaf (§Search), so the leaf is
smaller than the 4 KiB B+ tree leaf. §Gates benchmarks 32, 64 and 128.

### Invariants

- Entries are in key order. `meta[i].crit == crit(key[i-1], key[i])` for
  `i ≥ 1`.
- `fp(key[i])` and `entry_bytes` describe the record at `loc[i]`.
- Every `loc` in a published version names bytes already written to the data
  file. Every `loc` in the writer's transient names bytes written *or* bytes
  in the batch the writer is building (§Key resolution).
- Within one process, a `(file_id, offset)` pair names at most one record,
  ever. File ids come from `next_file_id_++` and are never reused; files are
  append-only; `resume()` trims only bytes no version references and then
  seals the file. This is what makes location equality mean record equality
  (§Sequence without a sequence field).

## Key resolution

Every operation that needs a key's bytes asks a resolver:

```cpp
// Returns the key of the record at loc. The span is valid until the next
// call on the same resolver. Throws on I/O failure or CRC mismatch.
struct KeyResolver {
  virtual auto key_at(Loc loc) -> std::span<const std::byte> = 0;
};
```

The tree takes the resolver as an argument to each operation that needs one
and never stores it; the tree module stays free of file I/O and the tests
drive it with an in-memory resolver.

The engine's resolver reads the header and key of the entry at `loc` through
the `DataFile` in the state's file registry, so a snapshot resolves against
the files it pins. One case needs more: phase 1 of a commit (validate and
apply) runs **before** the batch's `pwritev`
(`docs/commit_pipeline_design.md`), so the writer's transient can hold
locations whose bytes are not yet on disk: a batch that puts `k1` then `k2`
may land `k2`'s blind search on `k1`'s new record. The writer's resolver
first checks whether `loc` is in the active file at or past its written end,
and if so answers from the batch being built, whose ops it already holds in
offset order. Readers never see these locations, because the version is
published after `fdatasync`.

## Search

### Blind search in a leaf

A left-to-right pass finds the candidate. `c` is the candidate. `s` is the
crit bit of the last left turn still in force: the boundaries after it with
a crit bit at or above `s` are that node's right subtree, which the search
did not enter.

```cpp
auto blind_candidate(const Leaf &l, std::span<const std::byte> q) -> std::size_t {
  std::size_t c = 0;
  std::uint32_t s = kNoCrit;                        // above every crit bit
  for (std::size_t i = 1; i < l.count; ++i) {
    const auto p = l.crit(i);
    const bool on_path = p < s;
    const bool right = bit(q, p);
    c = (on_path && right) ? i : c;                 // branch-free; one pass
    s = on_path ? (right ? kNoCrit : p) : s;
  }
  return c;
}
```

This walks the Patricia trie that the sorted keys and their crit bits imply
without building it. A boundary with a smaller crit bit is an ancestor of
the boundaries between it and the previous smaller one. Turning right at a
boundary leaves every subtree to its left, so it clears `s`; turning left
starts skipping that boundary's right subtree. It is 63 iterations at most,
with no branch on data, over 256 contiguous bytes.

A first draft kept `s` across a right turn. That skips boundaries of the
subtree the search has just entered and returns a wrong candidate. A
brute-force check of this pass, of step 5 below, and of the insert and erase
rules, against sorted arrays of random keys (prefix keys and `\0` bytes
included, 23,000 key sets), found it. The same check belongs in the unit
tests (§Tests).

### Resolving the candidate

`find(q, resolver)` returns the entry for `q` or its insertion position:

1. Route to the leaf through the inner nodes, as today.
2. `c = blind_candidate(leaf, q)`. If the leaf is empty, the position is 0.
3. If `fp(q) != fp(c)`: `q` is not in the leaf. For a lookup that is the
   answer, with no I/O. An insert or `lower_bound` goes on to step 4, because
   it needs the position.
4. Read `key[c]` through the resolver. If it equals `q`, found at `c`.
5. Otherwise let `j = crit(q, key[c])`. The keys that share bits `[0, j)`
   with `c` are a contiguous run `[a, b)` around `c`: extend left and right
   while `crit > j`. Every key in the run has the same bit `j` as `c`, so if
   `bit(q, j)` is 1, `q` sorts after the run and its position is `b`;
   otherwise it is `a`.

Step 5 is the Patricia argument: `j` cannot be a crit bit on `c`'s path,
because the search would have tested it and followed `q`'s bit, not `c`'s.
So no boundary inside the run has crit `j`, and the boundaries at `a` and `b`
have crit bits below `j` (a boundary with crit `j` next to the run would be
on `c`'s path too).

A lookup costs one read when `q` is present or its fingerprint collides, and
none when it is absent. An insert or `lower_bound` costs one read unless the
leaf is empty.

## Algorithms

Everything is written once in `BuildSession`, as in the B+ tree (D11). Only
the leaf-level steps change; the path copying, `mutable_copy`, splits of
inner nodes and garbage sites are unchanged.

### Insert

With the position from step 5 and `j = crit(q, key[c])`:

- `q` after the run, at `b`: `crit(q) = j`, and the key now after `q` keeps
  its crit bit (`crit(q, key[b])` is the old `crit(key[b-1], key[b])`, which
  is below `j`).
- `q` before the run, at `a`: `crit(q)` is the old `crit(key[a-1], key[a])`
  (unused when `a == 0`), and `key[a]` gets `j`.

No other entry changes, and no key other than `key[c]` is read. The entries
are fixed-size, so an insert shifts the arrays above the position by one.

### Overwrite

`find` resolves `q` to itself: replace `loc`, `entry_bytes`. The crit bit
and fingerprint describe the key, which has not changed.

### Erase

Remove index `i`. If `i > 0` and `i + 1 < count`, the next key's crit bit
becomes `min(crit[i], crit[i+1])`: the first difference between two keys is
the smallest first difference between any adjacent pair between them. No
read.

### Split

Split at the middle, or at the append point for the rightmost leaf as the
B+ tree does. The crit bit at the split, `crit(left.last, right.first)`, is
already in the leaf, and it gives the separator's length: `cpl =
crit / 9`, separator `= right.first[0 .. cpl + 1)`. Building it needs the
bytes of `right.first`, **one read per split**. When the key being inserted
is `right.first`, its bytes are in hand and the read is skipped. With
64-entry leaves a split happens about once every 32 inserts in random order.

### Range erase (`del_range`)

Two seeks, `lower_bound(from)` and `lower_bound(to)`, one read each. Every
entry between the two positions is erased without reading its key, and
`entry_bytes` gives each one's live-bytes decrement. The disk cost of
`del_range` stays O(1) in the number of keys it removes.

### Iteration

The iterator stack is the B+ tree's. Advancing is in memory. What it yields
differs:

- **Value iterators** (`iter_from`, `riter_from`) read `entry_bytes` at
  `loc`, which is the read they do today, and take the key from the same
  bytes. No extra I/O.
- **Key iterators** (`keys_from`, `rkeys_from`) read the header and key of
  each entry: **one read per key**, where today there are none. The iterator
  owns the buffer the key is read into, so the span it yields lives until the
  next advance, as the lifetime rules require.

A seek costs one read (§Search).

### Bulk load and recovery

The bulk loader takes keys in ascending order, so it has every key's bytes
in hand: crit bits between neighbours, fingerprints and separators are
computed from the stream without a single read.

Recovery cannot merge the workers' trees the way `recovery_load_ranged` does
today, because merging two blind trees means comparing keys neither holds.
It merges the **hint files** instead, which are already sorted by key and
then by sequence descending (`docs/file_format.md`):

1. **Per file, in parallel.** Verify the CRC, as today. Collect the range
   tombstones at the head of the file. Record a fence every 4 KiB: the key and
   byte offset of the first entry that starts in each 4 KiB step. A hint file
   has no sync marker, so this pass is also the only safe way to find entry
   boundaries.
2. **Splitters.** Pool the fence keys and cut them into `R` ranges.
3. **Per range, in parallel.** A k-way merge over every hint file's slice of
   the range, each slice found by binary search over that file's fences and
   a forward scan to the first key in range. For each key, the highest
   sequence wins (`kde_newer`'s rules, including `SequenceOverlap`); a winning
   `Delete`, or a range tombstone with a higher sequence covering the key,
   drops it. Survivors feed the bulk loader and their `entry_bytes` feed the
   file's live bytes.
4. **Concatenate** the range runs with `BulkLoader::concat`.

The format document allows unsorted hint files; phase 1 sorts one in memory,
bounded by `max_file_bytes`, as hint generation does.

## Sequence without a sequence field

| Use of `sequence()` today | Replacement |
|---|---|
| W-W check and `ensure_unchanged` (`bytecask.cppm` §validate) | Compare the **locations** the snapshot and the head resolve to. Equal means unchanged, by the location invariant: the same record, or the same neighbour in both, which means the key is absent in both. Different means a write, or vacuum moved the record: read both entry headers and compare sequences. |
| Range guards and a range delete's conflict check | Walk the snapshot's and the head's leaves over `[from, to)` together. Identical leaf pointers are skipped whole, since the versions share them. A run whose locations match is unchanged. A mismatch reads headers, as above. |
| `lost_to` on a conflict | Read the head entry's header. Conflicts are the slow path. |
| Vacuum liveness (`vacuum_scan_and_copy`) | The record is live when the key resolves to `(source_file, entry_off)`. Vacuum holds the key, so the blind search lands on it if present, and location equality confirms it with no read. |
| Vacuum remap (`apply_vacuum`) | Remap when the key still resolves to its old location. The mapping gains the old offset. No read. |
| `apply_resume`, sequence-wins replay | Read the existing entry's header. `resume()` is rare and already reads the file. |
| Recovery resolution | From the hint streams, which carry sequences. |
| Debug invariant "next_seq > max key_dir sequence" | Debug-only walk that reads every header, or dropped. |

The location comparison makes the common path of every guard free of I/O:
the only reads are on keys that did change, or that vacuum moved in between.

## I/O per operation

| Operation | Today | Blind leaves |
|---|---|---|
| `get`, present | 1 read | 1 read (`entry_bytes` known, key checked in the same bytes) |
| `get`, absent | 0 | 0 (1 in 16.7M on a fingerprint collision) |
| `contains_key`, present | 0 | **1** |
| `put`, new key | 0 | **1**, plus 1 per split (about 1 in 32) |
| `put`, overwrite | 0 | **1** |
| `del`, present / absent | 0 / 0 | **1** / 0 |
| `ensure_unchanged`, W-W, unchanged | 0 | 0 |
| `del_range` | 0 | **2** |
| `iter_from`, per key | 1 | 1 |
| `keys_from`, per key | 0 | **1** |
| vacuum liveness and remap | 0 | 0 |

The reads land on records the writer or a reader touched recently, often in
the active file, which is resident in the page cache and in the buffer pool.

## Memory

Estimates, to be replaced by `memory_profile` (§Gates). A leaf entry is
16 B; fill is about 0.69 for random insert order and near 1.0 for ascending;
inner nodes add about 0.5 B/key.

| Key shape (from the B+ tree measurements) | B+ tree today | Blind leaves (est.) |
|---|---:|---:|
| prefixed UUIDv7, clustered, hash_prefixed | 33–34 | ~17 |
| binary (8 B) | 47 | ~20 |
| uniform `key_N` | 57 | ~27 |
| uuidv4_binary | 71 | ~24 |
| uuidv4_text, sha256_bin | 96 | ~24 |
| sha256_hex | 133 | ~24 |

Structured keys, which the B+ tree's prefix compression already handles
well, gain about 2×; long random keys gain 4–5×. The size no longer depends
on key length. HOT's reported 11–14 B/key is lower because its leaf value is
8 bytes; dropping `entry_bytes` would get this design to 12 B/key, at the
cost of a read per key in `del_range` and a second read in `get` for values
longer than a first guess. §Open questions keeps that variant open.

## Latency

A write that reads a key does so while the leader holds the write mutex, in
phase 1. On a warm page cache or buffer pool that is under a microsecond; on
a cold SATA SSD it is about 100 µs, and a batch of 64 cold reads is several
milliseconds of serial I/O before the `fdatasync`. The cost per write is
constant, one read, so latency stays predictable; its variance is the cache
hit ratio's.

The fix, if §Gates shows the need: resolve candidates *before* joining the
group, against the latest published version, and at apply time check that
the leaf the candidate came from is still the leaf the transient routes to
(same node pointer, or same tag). Only a key whose leaf changed in between is
re-read under the mutex. The reads then run in parallel across writers and
phase 1 stays in memory.

## Failure modes

- **A write can fail on a neighbour's record.** An insert reads the
  candidate's key, which belongs to another key. An I/O error or CRC failure
  there fails the write: `std::system_error` or `std::runtime_error`, as a
  failed read does today, and nothing is appended because phase 1 fails
  before phase 2. The engine does not enter the degraded state, since
  nothing was written. `CONTRACT.md` gains this case.
- **Key resolution during recovery cannot fail this way:** recovery reads
  hint files, not data entries.
- **A fingerprint collision is not a correctness risk.** A match is always
  confirmed by the key bytes; the fingerprint only skips reads on a mismatch.

## Engine interface

The blind tree cannot sit behind today's `KeyDirTree` alias. Its values have
no sequence, its lookups take a resolver, and its key iterators do I/O. The
engine therefore talks to the key directory through a narrow facade that
both trees implement:

```cpp
// What the engine needs from a key directory, in its own terms.
struct KeyDirHit { Loc loc; std::uint32_t entry_bytes; };

// find(key)                    -> optional<KeyDirHit>      verified match
// same_record(snap, head, key) -> bool                     guard fast path
// put / erase / erase_range    -> displaced KeyDirHit(s)    for live bytes
// sequence_of(hit)             -> u64                       header read on the blind tree
// iterators, snapshots, bulk load, size
```

For the B+ tree the facade is a thin wrapper: `sequence_of` reads the
`KeyDirEntry`, the resolver is never called. That facade is also the
pluggable key directory interface: the same engine over either tree, chosen
by a template parameter and, later, at `open` time through a variant of
engine instantiations, so the hot path stays inlined.

## Selection

Build-time, as the radix tree is: `BYTECASK_KEYDIR=blind`, and CI runs the
engine suite on it. A runtime `Options::key_directory` is a follow-up that
belongs to the pluggable-interface work, not to this tree.

## Tests

- **Tree, with an in-memory resolver** (`tests/blind_btree_test.cpp`):
  property test against `std::map` over random insert, overwrite, erase,
  `lower_bound`, forward and reverse iteration, `erase_range`. Key sets built
  to stress the encoding: keys that are prefixes of each other, embedded and
  trailing `\0`, long shared prefixes, 65,535-byte keys, single-bit
  differences at every bit of a byte. A resolver that counts calls asserts
  the I/O table above.
- **Crit-bit invariant**: debug `validate()` checks `crit(key[i-1], key[i])`
  against the stored value on every leaf of every test.
- **Persistence**: the B+ tree's snapshot, transient and `[accounting]`
  tests run on the blind tree; structural sharing is unchanged, so the
  numbers should be too.
- **Engine**: the full suite under `BYTECASK_KEYDIR=blind`, the `[model]`
  recovery tests included, with serial and parallel recovery checked for
  equal keys, values and `file_stats`.
- **Pending-batch resolution**: a batch whose later op's candidate is an
  earlier op's record in the same batch.
- **Neighbour failure**: fault injection on the read of a candidate's key;
  the write fails, nothing is appended, the engine is not degraded.

## Gates

In order; the design is abandoned or revised at the first gate that fails.

| # | Gate | Measure |
|---|---|---|
| G1 | Memory | `memory_profile` per key shape: at or below 25 B/key on every random shape, at or below the B+ tree on every shape |
| G2 | In-memory search | `map_bench` `Get` and `LowerBound` with a resolver that does no I/O: within 2× of the B+ tree |
| G3 | Point reads | `engine_bench` `Get` and `GetMT`: within 10% |
| G4 | Writes | `engine_bench` `Put` NoSync and Sync, `MixedBatch`: record the cost; Sync within 10% |
| G5 | Recovery | 10M keys, 16 threads: within 1.5× of the B+ tree |

G1 is the reason to build this; if it fails, nothing else matters.

## Plan

1. Leaf node, blind search, insert, erase, split, iterators, bulk loader, in
   `bytecaskdb/blind_btree.cppm`, sharing the inner node and `BuildSession`
   code with `btree.cppm` rather than copying it. Tree tests with the
   in-memory resolver. G1, G2.
2. The key directory facade in the engine, with the B+ tree behind it and no
   change in behaviour. Engine suite green on both existing trees.
3. The blind tree behind the facade: resolver, pending-batch resolution,
   location-based guards, vacuum by location. Engine suite under
   `BYTECASK_KEYDIR=blind`. G3, G4.
4. Recovery from sorted hint streams. Model tests. G5.
5. If G4 shows the reads under the mutex: resolve before joining the group.

## Open questions

- **Leaf size.** 64 entries is a guess that trades search length against
  inner node count. Benchmark 32, 64, 128.
- **12-byte entries.** Drop `entry_bytes` for `loc` plus fingerprint plus
  crit bit: HOT's density, with a read per key in `del_range` and a
  speculative first read in `get`.
- **`contains_key`.** One read per present key is a regression for callers
  that use it as a cheap existence test. A 24-bit fingerprint cannot answer
  "present" on its own; a caller that tolerates 1-in-16.7M false positives
  could have a separate `probably_contains`, but that is a new API, not a
  change to this one.
- **Hint-stream recovery for the other trees.** Phase 1–4 of §Bulk load and
  recovery never builds per-worker trees and may beat today's recovery on the
  B+ tree too. Worth measuring once it exists.
- **The facade's shape.** Whether the variant-at-open selection fits the
  engine's module structure, and what it costs in build time, is part of the
  pluggable-interface work.

## References

- D. R. Morrison, *PATRICIA — Practical Algorithm To Retrieve Information
  Coded in Alphanumeric*, JACM 1968.
- P. Ferragina, R. Grossi, *The String B-tree: A New Data Structure for
  String Search in External Memory and Its Applications*, JACM 1999.
- P. Bohannon, P. McIlroy, R. Rastogi, *Main-memory index structures with
  fixed-size partial keys*, SIGMOD 2001.
- R. Binna, E. Zangerle, M. Pichl, G. Specht, V. Leis, *HOT: A Height
  Optimized Trie Index for Main-Memory Database Systems*, SIGMOD 2018.
- H. Lim, B. Fan, D. G. Andersen, M. Kaminsky, *SILT: A Memory-Efficient,
  High-Performance Key-Value Store*, SOSP 2011.
- `docs/persistent_btree_design.md`: node layout, `BuildSession`,
  `VersionChain`, memory measurements used above.
