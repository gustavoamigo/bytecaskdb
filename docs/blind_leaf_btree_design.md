# Blind-leaf B+ tree key directory — design

> **Status: steps 1–3 built and measured; experimental.** It describes a
> third key directory beside the B+ tree and the radix tree, for deployments
> where the number of keys, not the value data, is what runs out of RAM. The
> tree (`bytecaskdb/blind_btree.cppm`) is tested, and `BYTECASK_KEYDIR=blind`
> builds the engine on it with recovery going through a B+ tree (§Steps 2–3
> results). §Step 1 results has G1 and G2; the numbers elsewhere in this
> document are the estimates the proposal started from.

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

A bit position is written `byte << 4 | r`: `r = 0` is the byte's
continuation bit, `r = 1..8` its bits from the most significant. Positions
written this way compare in bit order, and splitting one is a shift and a
mask rather than a division by 9. Bit `p` of key `k` is:

```cpp
auto bit(std::span<const std::byte> k, std::uint32_t p) -> std::uint32_t {
  const std::size_t i = p >> 4;
  const auto r = p & 15u;
  if (i >= k.size()) return 0;                      // past the end: 0
  if (r == 0) return 1;                             // a byte is present
  return (std::to_integer<std::uint32_t>(k[i]) >> (8 - r)) & 1u;
}
```

A shorter key has `0` where a longer key with the same prefix has its
continuation `1`, so it sorts first, which is what `std::ranges::lexicographical_compare`
does on bytes. `crit(a, b)` is the first `p` where `bit(a, p) != bit(b, p)`.
Two distinct keys first differ at byte 65,534 at the latest, so the largest
crit bit is `65,534 << 4 | 8`, and 20 bits hold it.

The byte-level common prefix used for separators is `crit(a, b) >> 4`.

## Leaf layout

```
  ┌──────────────┬────────────────────────────┬──────────────────────────────────┐
  │ header 104 B │ meta: u32 × capacity       │ entry: u64 loc, u32 size × cap   │
  └──────────────┴────────────────────────────┴──────────────────────────────────┘

  meta  = crit:20 | fp_lo:12        crit of this key against the previous key in the leaf;
                                    unused for index 0
  loc   = file_id:20 | offset:32 | fp_hi:12
  size  = entry_bytes:29 | 0:3      header + key + value + CRC of the data entry
```

The header is the B+ tree's `Node` header (tag, capacity, count,
`last_pos`, `is_leaf`), so a blind leaf is a `Node` whose bytes after the
header are these arrays instead of slots and a heap. That is what lets the
version chain, `BuildSession` and the inner nodes handle it unchanged:
children stay `Node *`, `is_leaf` tells a leaf apart, and reclamation never
looks inside a leaf. `prefix_len`, `heap_floor`, `dead_bytes` and the 64-byte
search hints are unused in a blind leaf. The header is 104 bytes rather than
the 32 the proposal assumed, which costs about 1.5 B/key at 0.7 fill with
73-entry leaves (§Step 1 results). The arrays are structure-of-arrays so the
search reads only `meta`.

The 24-bit fingerprint (`fp_lo`, `fp_hi`) is a hash of the full key, taken
when the key is inserted and never recomputed: a multiply-xor over 8-byte
little-endian words, inlined, since every lookup and write computes one. It is not used for ordering.
It lets a lookup reject a candidate that is not the key without reading
anything, with a false-match rate of 1 in 16.7 million.

`entry_bytes` replaces `value_size`. `get` reads exactly that many bytes, and
live-bytes accounting needs exactly that number (`entry_size(key, value)`)
without knowing the key length. The largest entry, a 256 MiB value and a
65,535-byte key, fits in 29 bits.

What leaves the key directory: the 48-bit `sequence` and the key bytes.
§Sequence without a sequence field shows where each use of `sequence()` goes.

The leaf size is a template parameter, in bytes, and the capacity follows
from it: `(bytes − 104 − 4) / 16` entries. Sizes are jemalloc size classes so
no allocation is rounded up: 640 bytes hold 33 entries, 1,280 hold 73 and
2,560 hold 153. Blind search is linear in the leaf (§Search), so the leaf is
smaller than the 4 KiB B+ tree leaf.

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

The crit bit at the split, `crit(left.last, right.first)`, is already in the
leaf, and it gives the separator's length: `cpl = crit >> 4`, separator
`= right.first[0 .. cpl + 1)`. Building it needs the bytes of
`right.first`, **one read per split**. When the key being inserted is
`right.first`, its bytes are in hand and the read is skipped. With 73-entry
leaves a split happens about once every 25 inserts in random order.

Where to split. An insert is *ascending* when it lands one or two positions
after the leaf's previous insert (`last_pos`, as in the B+ tree). Two
positions count because a stream of new keys often runs past one existing key
per step: `key_123459` then `key_123460` skip over `key_12346`. In order of
preference:

1. Ascending, appending at the end of the leaf: the new key starts the right
   leaf, so the left one stays full.
2. Ascending, and the next key shares at least 8 bytes fewer with the new
   key than the previous key does (another key family, read off the two crit
   bits): the new key ends the left leaf. The stream goes on to fill leaves
   of its own instead of dragging the other family along.
3. Ascending, otherwise: at the insert point, but never left of the middle.
   A blind leaf's capacity is a count, not bytes, so splitting a full leaf
   near its start leaves the right half full again, and every following
   insert split off a one- or two-key leaf (`uniform` keys filled leaves to
   0.54 that way).
4. Descending (`pos == 0` after an insert at 0): after the first entry.
5. Otherwise the middle.

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

## Step 1 results

Built: `bytecaskdb/blind_btree.cppm`, tested by `tests/blind_btree_test.cpp`
(a brute-force check of the search over 23,000 single-leaf key sets with
prefix keys, `\0` bytes and single-bit differences; a model test against
`std::map` with snapshots; read counts per operation; 65,535-byte keys).
Undoing the right-turn reset in the search fails five of the seven test
cases.

What the tree shares with `btree.cppm`: the node header and inner nodes,
`BuildSession` (ownership, discard, path copying, inner-node splits and
`make_root`, through a descent that takes the leaf step as a callback), the
level building and publishing of `BulkLoader`, `ChainTraits` and the
`VersionChain`. What it adds: the leaf layout, the search, the leaf steps of
insert, overwrite, erase and split, its iterator, its handle types and a
loader that fills blind leaves. `map_bench` shows the B+ tree unchanged by
the refactor that exposes those pieces.

Measured on a 4-vCPU cloud VM (Intel Xeon @ 2.10 GHz), where repeated runs
vary by up to 30%; `map_bench` figures are medians of three.

### G1 memory

`BC_INDEX_ONLY=… memory_profile`, 1M keys inserted in batches of 100 as the
engine does, jemalloc heap per key. Leaf fill in parentheses.

| Key shape | B+ tree | Blind 640 (33) | Blind 1280 (73) | Blind 2560 (153) |
|---|---:|---:|---:|---:|
| prefixed | 33.9 | 21.4 (1.00) | 18.4 (1.00) | 17.2 (1.00) |
| hash_prefixed | 33.5 | 20.9 (1.00) | 18.2 (1.00) | 17.0 (1.00) |
| clustered | 33.3 | 20.6 (1.00) | 18.1 (1.00) | 17.0 (1.00) |
| zipfian | 35.0 | 21.4 (0.98) | 18.6 (0.98) | 17.4 (0.98) |
| uuidv7 | 58.5 | 21.1 (1.00) | 18.3 (1.00) | 17.1 (1.00) |
| uuidv7_binary | 41.8 | 20.6 (1.00) | 18.1 (1.00) | 17.0 (1.00) |
| binary | 47.3 | 27.5 (0.74) | 24.5 (0.73) | 22.8 (0.74) |
| uniform | 56.5 | 28.8 (0.72) | 24.8 (0.73) | 23.3 (0.73) |
| incremental | 56.3 | 28.8 (0.72) | 24.8 (0.73) | 23.3 (0.73) |
| many_partitions | 62.6 | 30.7 (0.67) | 27.0 (0.67) | 25.4 (0.67) |
| mixed | 75.9 | 26.5 (0.78) | 23.5 (0.77) | 22.1 (0.77) |
| uuidv4_binary | 71.5 | 29.6 (0.69) | 26.0 (0.69) | 24.3 (0.70) |
| sha256_bin | 96.1 | 29.6 (0.70) | 25.9 (0.69) | 24.0 (0.71) |
| uuidv4_text | 96.1 | 29.8 (0.69) | 25.9 (0.69) | 24.8 (0.68) |
| uuidv4_prefixed | 96.2 | 29.6 (0.69) | 26.1 (0.69) | 24.5 (0.69) |
| sha256_hex | 132.8 | 29.6 (0.69) | 25.9 (0.69) | 24.3 (0.70) |

- Below the B+ tree on every shape at every leaf size: 1.8–2.0× smaller on
  structured keys, 2.8–5.1× on random ones at 1,280 bytes.
- "At or below 25 B/key on every random shape" holds at 2,560 bytes
  (24.0–24.8), misses by about 1 B/key at 1,280 (25.9–26.1) and by 4.6 at
  640. At 1,280 the 104-byte shared header costs 2.1 B/key at 0.69 fill;
  the proposal's 32-byte header would cost 0.6, which puts the random shapes
  at about 24.5. The miss is the price of sharing the B+ tree's header, not
  of the design.
- `many_partitions` is the worst structured shape: its second pass adds one
  key after every existing key, so every full leaf must end up holding twice
  its capacity, three leaves at two thirds each. A leaf with a byte budget
  has the same problem in a milder form.

### G2 in-memory search

`map_bench`, `generate_uniform_keys`, with a resolver that returns a span
into an in-memory vector (so a key read costs one cache miss, not I/O).

| Benchmark | B+ tree | Blind 640 | Blind 1280 | Blind 2560 |
|---|---:|---:|---:|---:|
| Get/1000 | 67 ns | 127 (1.9×) | 129 (1.9×) | 229 (3.4×) |
| Get/10000 | 42 ns | 82 (1.9×) | 113 (2.7×) | 359 (8.5×) |
| Get/100000 | 76 ns | 159 (2.1×) | 147 (1.9×) | 214 (2.8×) |
| GetAbsent/100000 | 71 ns | 153 (2.2×) | 159 (2.2×) | 229 (3.2×) |
| LowerBound/1000 | 134 ns | 149 (1.1×) | 189 (1.4×) | 242 (1.8×) |
| LowerBound/10000 | 101 ns | 109 (1.1×) | 156 (1.5×) | 361 (3.6×) |
| LowerBound/100000 | 126 ns | 182 (1.4×) | 212 (1.7×) | 275 (2.2×) |

- `LowerBound` passes (within 2×) at 640 and 1,280 bytes. `Get` is at the
  line at 640 and 1,280 (1.9–2.7×), and fails at 2,560.
- In absolute terms a blind `Get` costs 40–90 ns more than a B+ tree `Get`.
  The README's engine `Get` is 728 ns at p50, so if nothing else changed the
  difference would be about 10% of it, which is G3's whole budget.
- `GetAbsent`, which never reads a key, costs the same as `Get`: the time is
  the in-leaf scan, not the key read. It grows by about 2 ns per entry
  (33 → 73 → 153), so the loop is bound by its instruction count (about 20
  µops per boundary), not by the dependency on `s`. Three attempts did not
  move it: shift-and-mask positions instead of division by 9, an inlined
  fingerprint instead of a CRC-32C library call (both kept, as simpler), and
  arithmetic masks instead of conditional moves (slower, dropped). Making it
  faster takes a different in-leaf search, such as HOT's partial keys
  compared with SIMD.

### Writes and iteration (context, no gate)

| Benchmark | B+ tree | Blind 640 | Blind 1280 |
|---|---:|---:|---:|
| TransientInsertBatch/100000 (100 new keys) | 20.9 µs | 35.2 (1.7×) | 38.4 (1.8×) |
| TransientSet/100000 | 12.9 ms | 20.8 (1.6×) | 19.9 (1.5×) |
| TransientUpdate/100000 (overwrite all) | 6.3 ms | 13.7 (2.2×) | 20.2 (3.2×) |
| Iterate/10000 (with keys) | 76.5 µs | 42.2 (0.6×) | 35.3 (0.5×) |

An overwrite reads the key it replaces, which the B+ tree never does; that
is the 2–3× on `TransientUpdate`. Iteration looks faster only because the
benchmark's resolver hands out a span into memory while the B+ tree copies
each key into the iterator's buffer; in the engine a blind `keys_from` does
one read per key.

### Leaf size

1,280 bytes (73 entries) is the middle ground: 640 is up to 30% faster to
search and 3–4 B/key larger; 2,560 is 1.5 B/key smaller and 1.5–3× slower.
With a leaner leaf header, 1,280 would also pass G1.

## Steps 2–3 results

Built as the easiest version that runs the whole engine, not the one the
sections above describe in full:

- **The facade** is a set of `kd_*` functions in `bytecaskdb/internals.cppm`
  that take a `KeyDirCtx` (the version's file registry, and the writer's
  records not yet written) and forward to the B+ or radix tree unchanged.
  `bytecaskdb/bytecask.cppm` calls nothing else. A put is now one `upsert`
  on every tree, where it was a `get` and a `set`.
- **Sequences are read, not compared by location.** The read that confirms a
  key returns the whole entry, header included, so `kd_get` returns a full
  `KeyDirEntry` and every guard, vacuum's remap and `resume()` run unchanged.
  §Sequence without a sequence field is not implemented: a guard on an
  unchanged key costs a read.
- **The leaf's size field holds the value size**, not `entry_bytes`: the
  data file API reads an entry given its value size, and every live-bytes
  update already has the key length.
- **Records not yet written** are resolved from `TransientEngineState`'s
  `pending_` map, filled as each put or ingested put is applied.
- **Recovery builds the B+ tree and converts it** (`key_dir_from_recovered`).
- `DB::get` takes the value from the read that confirmed the key.

Tests: the engine suite passes on all three trees (2,784 cases on the B+ and
radix trees, 2,750 on the blind tree). The 34 it does not run are the
`validate_preconditions` and `apply_resume` unit tests, which build an
`EngineState` by hand with entries that point at no data file; the paths
they cover run through the DB-level tests.

`engine_bench`, 1M keys, the default buffer-pool back end, CRC off on reads,
medians of three interleaved runs of each binary on the VM above. "Base" is
the engine before step 2; "B+" is the B+ tree behind the facade.

| Benchmark | Base | B+ | Blind | B+ / base | Blind / B+ |
|---|---:|---:|---:|---:|---:|
| Get | 279 ns | 249 ns | 402 ns | 0.89 | **1.62** |
| GetMT, 2 threads | 258 ns | 276 ns | 409 ns | 1.07 | **1.48** |
| GetMT, 4 threads | 270 ns | 273 ns | 404 ns | 1.01 | **1.48** |
| GetMT, 4 threads, pread | 655 ns | 743 ns | 794 ns | 1.13 | 1.07 |
| Range50 | 2.86 µs | 2.53 µs | 2.92 µs | 0.88 | 1.16 |
| Put, NoSync | 5.20 µs | 4.77 µs | 5.30 µs | 0.92 | 1.11 |
| Put, Sync | 202 µs | 248 µs | 179 µs | 1.23 | 0.72 |
| Del, Sync | 208 µs | 188 µs | 193 µs | 0.91 | 1.03 |
| MixedBatch, Sync | 415 µs | 545 µs | 608 µs | 1.31 | 1.12 |
| Recovery, 4 threads | 60 ms | 57 ms | 100 ms | 0.94 | 1.75 |

- **The facade costs the B+ tree nothing.** Every CPU-bound row is equal or
  faster. The Sync rows swing ±25% between runs of the same binary; five
  more interleaved MixedBatch runs put the B+ build at 328–455 µs against
  405–517 µs for the base, faster in all five.
- **G3 (point reads within 10%) fails.** A warm `Get` costs 150 ns more.
  The in-leaf scan accounts for about 80 ns of it (§G2). The rest is
  probably the read, not measured apart: it looks the header up in the pool
  and then the entry, two lookups where the B+ tree's `read_value` does
  one. Where the read itself is expensive the gap shrinks:
  1.07× through `pread`. Nothing here is a disk read; on a cold cache both
  trees pay the same one.
- **G4 (writes): NoSync +11%, Sync within noise.** A put reads one record,
  from the page cache or the pool, under the write mutex.
- **Recovery is 1.75×**, the cost of building a B+ tree and converting it;
  peak memory at open is the B+ tree's. Step 4 replaces this.

## Revision 2

What steps 1–3 measured changes the plan. The tree already meets its memory
goal against the B+ tree (17–26 B/key, 2–5× smaller); what it misses is G3
(a warm `Get` is 1.62× the B+ tree's) and the proposal's 25 B/key on random
keys at 1,280-byte leaves. Measured, the per-key cost on random keys is
16 B / 0.69 fill + 2.1 B of the 104-byte shared header + 0.5 B of inner
nodes ≈ 26 B, so there are three levers — the entry, the fill and the header
— and the read path has one lookup too many. This revision adds seven
changes, R1–R7, and moves location-based guards and resolving outside the
commit group to later (R8).

### Benchmark baseline

The buffer pool is the reference back end: `engine_bench`'s `ByteCaskDB/*`
rows (`IoBackend::BufferPool`, pool sized to hold the dataset, warmed before
measuring). G3 and G4 are judged on those rows. The `_Pread` and `_Mmap`
rows are recorded for context: they bound how much of the gap a system call
hides. `memory_profile BC_INDEX_ONLY=…` stays the reference for G1 and
`map_bench` for G2. Every change below reports the B+ tree's rows before and
after as well, since R3 and R4 touch code the B+ tree runs.

### R1. One lookup per record read

The engine's `KeyReader` calls `DataFile::read_entry`, which on the pool back
end fetches the header (one frame lookup and a copy) and then the whole
entry (a second lookup and a second copy). `lend_entry` already serves an
entry with one `pool.view()` and no copy. The resolver switches to a lending
read, and one that takes the key and value sizes from the header rather than
from the caller, which R2 needs:

```cpp
// Header, key and value of the record at offset, sizes from its header.
// Spans valid until the next call with the same io_buf and lease.
virtual auto lend_record(Offset offset, bool verify,
                         std::vector<std::byte> &io_buf,
                         FrameLease &lease) const -> DataEntryView = 0;
```

- **Buffer pool:** one `view()` of the frame, sizes parsed from the header in
  place; a copy only when the entry straddles a frame boundary.
- **mmap:** the mapping, no copy.
- **pread:** one speculative read of the header, the key and a guessed value
  length (the same over-read `read_entry_unverified` does today), then one
  more `pread` for the remainder if the value is longer. The guess is a
  per-file running estimate of recent value sizes, capped at a few KiB.

`kd_read_value` takes the value from the same lease. Expected: the part of
the 150 ns `Get` gap that is not the in-leaf scan (§Steps 2–3 results
estimates it at about 70 ns). This change is independent of the rest and
helps the current 16-byte entry as much as the 12-byte one.

**Done.** `lend_record(offset, value_size_hint, verify, io_buf, lease)`
replaces `lend_entry`: the entry iterators pass the size they know as the
hint, the blind reader passes its own. Measured (1M keys, buffer pool, medians
of three interleaved runs): blind `Get` 2.04 → 2.21 M ops/sec (+8%), still
0.61 of the B+ tree — the leaf scan is most of what is left. The B+ tree's
`Get`, `Range50` and `GetMT` are unchanged within noise over five more
interleaved runs.

### R2. 12-byte leaf entry

Drop the size field:

```
  meta = crit:20 | fp_lo:12                     u32
  loc  = file_id:20 | offset:32 | fp_hi:12      u64
```

With R1 no read needs the size in advance. The places that used it:

| Use | With the size field | Without it |
|---|---|---|
| `get`, value iterators | size tells the read length | the header in the same lookup (R1) |
| put / erase, live bytes of the displaced record | size in the leaf | the record was just read to confirm the key; its header has it |
| `del_range`, live bytes of each erased key | size in the leaf | estimated (R3), no read |
| vacuum remap, resume | size from the scan | unchanged |

Memory: 12 B per entry instead of 16, before fill and header. Leaf capacity
at 1,280 bytes goes from 73 to about 98 entries, which lengthens the in-leaf
scan by a third unless R6 lands first or the leaf shrinks to 960 bytes
(about 71 entries).

**Done.** `BlindRef` is `{file_id, offset}`. Until R3, a range delete reads
each erased key's record for its live bytes. A put or an erase takes the
displaced record's value size from the read that confirmed the key (the
reader checks it was that record); records the batch has not written carry
their value size in the pending map. Measured with `memory_profile`, 1M keys,
B/key:

| Shape | 640 (44 entries) | 1,280 (97) | 2,560 (204) | before R2, 1,280 (73) |
|---|---:|---:|---:|---:|
| prefixed | 16.0 | 13.9 | 12.9 | 18.4 |
| uuidv7 | 15.8 | 13.8 | 12.8 | 18.3 |
| uniform | 21.4 | 18.8 | 17.5 | 24.8 |
| sha256_hex | 22.2 | 19.6 | 18.6 | 25.9 |
| uuidv4_binary | 22.1 | 19.7 | 18.9 | 26.0 |
| many_partitions | 23.0 | 20.3 | 19.1 | 27.0 |
| mixed | 20.0 | 17.6 | 16.6 | 23.5 |

The longer leaf costs search time: `map_bench` `Get` at 1,280 bytes is
184–226 ns against the B+ tree's 55–99 ns on this run (2.3–3.7×), and at 640
bytes 108–181 ns (1.8–2.0×). R6 is what has to pay for it.

### R3. `live_keys` exact, `live_bytes` estimated for range deletes

`FileStats` gains `live_keys`. Every path keeps it exact, including
`del_range`, because every leaf entry names its record's `file_id`: a range
delete knows which file each erased entry lived in without reading it.
`live_bytes` stays exact everywhere except `del_range`, which subtracts the
file's average live entry, `live_bytes / live_keys`, per erased entry; a file
whose `live_keys` reaches 0 gets `live_bytes = 0`. A later exact decrement
that exceeds an estimated total clamps at 0.

Two things must change with it, on every tree:

- **Vacuum's empty-file fast path** (`live_bytes == 0` → unlink without a
  scan) tests `live_keys == 0`. On an estimate, `live_bytes` could reach 0
  with keys still pointing into the file, and the fast path would delete
  live data.
- **`validate_state_consistency`** checks `live_keys` exactly, and
  `live_bytes` exactly only for files no range delete has estimated since
  their last exact count. `FileStats` carries that as a flag, cleared when
  the count is exact again.

Exact again at recovery (recomputed from the hint files, which carry value
sizes) and when vacuum compacts the file. The error is bounded by how far the
erased keys' sizes are from the file's average, and it steers only which file
vacuum picks and when; compaction decides liveness per record, by location.
The `[model]` recovery tests keep comparing exact stats.

### R4. A 40-byte leaf header

A blind leaf is a `btree_detail::Node` whose 104-byte header includes the B+
tree's 64-byte search hints, which a blind leaf never uses: 1.4–2.1 B/key.
Move `hints` out of the common header into the B+ tree's slotted layout
(stored after the header, as the prefix is), so every node starts with the
same 40-byte header (tag, first child, capacity, count, prefix length,
heap floor, dead bytes, last insert position, leaf flag) and a blind leaf's
arrays follow it directly. The B+
tree's layout changes by position only; `map_bench` and `memory_profile` on
the B+ tree must show no change. About −1.3 B/key on random keys, −0.9 on
structured.

**Done.** `Node` holds only the 40-byte header (a `static_assert` keeps it
there); `hints()` points at the 64 bytes after it, and the prefix and slots
follow as before, so a B+ node's bytes are where they were. The B+ tree's
`memory_profile` heap is byte-for-byte unchanged. Its `map_bench` `Get/100000`
measured a median of 93.3 ns before and 97.1 ns after over eight interleaved
rounds (ranges 91–99 and 92–104, slower in five of the eight): at most about
4%, not separable from this machine's noise. Blind leaves at 1,280 bytes hold
103 entries; 1M keys take 18.5 B/key on random keys (was 19.6) and 13.0–13.1
on structured ones (was 13.8–13.9).

### R5. Fuller leaves

Random inserts fill leaves to 0.69, ordered ones to 1.0.

1. **Bulk loads leave slack.** Recovery bulk-loads leaves 100% full, and
   the first random insert into each one splits it into two halves, so a
   freshly opened tree drops below 0.69 before it recovers. Loading to 0.8
   avoids the dip. One constant.
2. **Redistribute before splitting** (B*-style): move entries into a sibling
   with room and split two full leaves into three. Fill about 0.8. For blind
   leaves it costs about three key reads (the crit bit between the sibling's
   last entry and the moved first one, and the new separator) and a path copy
   of the sibling. Built only if (1) and the measured fill leave the random
   shapes above target.

**Measured, and (1) is a trade-off rather than a fix.** `BlindBulkLoader`
takes a fill, or a range of fills spread over successive leaves with a
golden-ratio sequence. `memory_profile
BC_INDEX_ONLY=blind_growth BC_BULK_FILL=…` bulk-loads 1M keys of a shape and
then inserts random keys of it. B/key:

| Keys, load fill | at load | +1% | +5% | +10% | +25% | +50% |
|---|---:|---:|---:|---:|---:|---:|
| random, 1.0 | 12.9 | 20.9 | 24.6 | 23.7 | 20.8 | 17.4 |
| random, 0.8 | 16.3 | 16.2 | 15.5 | 14.8 | 17.9 | 21.9 |
| random, 0.6–1.0 spread (recovery) | 16.3 | 16.4 | 17.2 | 18.0 | 19.3 | 19.6 |
| random, 0.5–1.0 spread | 17.3 | 17.5 | 18.0 | 18.5 | 19.3 | 19.1 |
| structured (`uniform`), 1.0 | 12.9 | 13.0 | 13.2 | 13.5 | 14.1 | 14.9 |
| structured (`uniform`), 0.6–1.0 spread | 16.3 | 16.3 | 16.3 | 16.4 | 16.6 | 16.8 |

Leaves loaded full all split within the first few percent of random
writes: the directory nearly doubles (12.9 → 24.6 B/key) before it settles.
A single slack value only moves that cliff (0.8 reaches it at +25%). A spread
removes it: the peak stays at the steady state, 19.6. But keys written in
order never split the leaves they loaded into, so for them the slack is paid
for good (16.3–16.8 against 12.9–14.9). Recovery loads with the 0.6–1.0
spread: the split storm is a burst of allocation on the write path and a
memory peak to size RAM for, and predictable latency ranks above the smaller
footprint. There is no `Options` field; if a workload needs the dense load,
the merged hint stream carries each key's sequence, so the loader could fill
full the ranges whose keys were written in key order. (2) is not built, and
is not planned: it needs the shared descent to reach
a leaf's sibling, a change to the B+ tree's insert path, for the 0.5 B/key by
which insert-built random trees miss G1 (18.5 against 18).

### R6. Faster in-leaf search

Tracked in #156. First the two-pass scan (extract the query's bits for every
boundary, then walk the crit bits); then a top-of-trie index of about 16
bytes in the leaf header (the root boundary and the next three levels'),
which narrows the scan to the 5–10 entries of one subtree. R6 is what makes
larger leaves affordable, and larger leaves are what makes R2 and R4 pay off.

**Done: the index, not the two-pass scan.** Each leaf keeps `top[15]`, one
byte per slot, after the header: the root boundary of each range of the
leaf's first four trie levels, in heap order, rebuilt after every change to
the leaf's entries (four passes over its crit bits). A search takes four bit
tests down it and scans only the range it lands in. The index costs 16 bytes
per leaf (a 1,280-byte leaf holds 101 entries). `validate()` recomputes it;
swapping the two children in the walk fails six of the seven tree tests.

`map_bench` at 1,280 bytes, B+ tree in parentheses: `Get` 80 / 81 / 111 ns
at 1k / 10k / 100k keys (71 / 54 / 89), was 184 / 207 / 226; `LowerBound`
124 / 113 / 141 (152 / 128 / 166) — faster than the B+ tree. At 2,560 bytes
four levels leave ranges of about 13 entries and `Get/10000` stays at
231 ns, so 1,280 stays the leaf size.

`engine_bench`, 1M keys, buffer pool, medians of three interleaved runs,
ops/sec as a fraction of the B+ tree's: `Get` 0.82 (0.62 before R1),
`GetMT` 0.82–0.84, `Range50` 0.88, `Put` NoSync 0.91, through `pread` 0.95.
G3 still misses its 10%.

### R7. Recovery from sorted hint streams

Step 4, unchanged: removes the build-and-convert through the B+ tree (1.75×
the B+ tree's recovery time, and its peak memory at open).

**Done** (`DB::recovery_load_streams`, used by the blind build). Per file in
parallel: range tombstones, sequence bounds, and a fence every 4 KiB, placed
only on the first entry of a key so a seek never skips an older duplicate.
Splitters come from the pooled fences; each range merges every file's slice
(seeking to its last fence below the range) straight into a
`BlindBulkLoader`, and `BlindBulkLoader::seal`/`concat` join the ranges
through the B+ tree's `BulkLoader::concat_into`. Every file is merged in
every range, so no tombstone map crosses threads; the newest entry of a key
wins through `kde_newer`, so `SequenceOverlap` is still thrown. No
intermediate tree is built. Seeking to the first fence at or above the range
instead fails the `[model]` and recovery tests.

`engine_bench` `Recovery`, 1M keys, three interleaved runs: 1 thread
0.143–0.149 s against the B+ tree's 0.203–0.225 s (0.255–0.260 s through
the B+ tree and conversion); 4 threads 0.053–0.056 s against 0.068–0.075 s
(0.119–0.125 s). G5 passes: the blind tree now recovers faster than the B+
tree. §Open questions asked whether the same path would speed up the B+
tree; it is not measured yet.

### R8. Later

Location-based guards (§Sequence without a sequence field) and resolving
candidates before joining the commit group (§Latency). Neither affects G1–G4
on the benchmarks above; both matter for guard-heavy and cold-cache
workloads.

### Where `Get` loses (G3 profile)

`engine_bench` `Get`, 1M keys, buffer pool, under callgrind with collection
limited to `DB::get` (1.3M calls; `-march=x86-64-v3`, since valgrind cannot
decode AVX-512). Per call:

| | B+ tree | Blind |
|---|---:|---:|
| Instructions | 2,049 | 2,519 |
| Simulated LL misses | 5.46 | 5.61 |
| Node searches | 4 (3 inner + leaf), 324 instr. each | 3 inner, 374 each |
| Leaf search | (in the 4th node search) | 453: 4 index tests, then ~10 entries scanned at ~27 instr. each |
| Fingerprint of the query | — | ~100, most of it assembling the tail word byte by byte |
| Record read and copy | `read_value` | `key_at` + `lend_record`: about the same |

Cache misses are level; the gap is ~470 instructions, which at this
machine's IPC is the ~20 ns by which the blind `Get` trails. Most of it is the
leaf: the scan the index leaves (about 10 entries, not the 4–5 a balanced
trie of ~70 keys would give, since a search lands more often in the larger
ranges) and the fingerprint. Two changes aim at it:

- A fifth index level: `top[31]`, 32 bytes, halves the scan (~−140
  instructions) for 16 more bytes per leaf, about 0.2 B/key.
- The fingerprint's tail read as one unaligned 8-byte load (the key's last
  eight bytes, overlapping the previous word) when the key has at least eight
  bytes, instead of a byte loop (~−50).

Together about 40% of the gap. The rest is the inner nodes (374 against 324
instructions each; the same code on different separators) and the resolver's
bookkeeping.

### Revised targets

Estimates from the arithmetic above, to be replaced by measurements as each
change lands:

| | Today | After R2 + R4 | After R2 + R4 + R5 |
|---|---:|---:|---:|
| Random keys, B/key | 25.9 | ~19 | ~16–17 |
| Structured keys, B/key | 17–18 | ~13–14 | ~13–14 |

| Gate | Target | Today |
|---|---|---|
| G1 | ≤ 18 B/key random, ≤ 14 structured (`memory_profile`, 1M keys) | 25.9 / 17–18 |
| G2 | `map_bench` `Get` within 2× of the B+ tree | 1.9–2.7× |
| G3 | `engine_bench` `Get`, `GetMT` within 10% (buffer pool) | 1.48–1.62× |
| G4 | `Put` NoSync within 10%, Sync within noise | +11% / noise |
| G5 | Recovery within 1.5× | 1.75× (through the B+ tree) |

## Plan

Done:

1. Leaf node, blind search, insert, erase, split, iterators, bulk loader, in
   `bytecaskdb/blind_btree.cppm`, sharing the inner node and `BuildSession`
   code with `btree.cppm`. Tree tests with the in-memory resolver. G1, G2.
2. The key directory facade (`kd_*`) in the engine, with the B+ tree behind
   it and no change in behaviour. Engine suite green on both existing trees.
3. The blind tree behind the facade, with sequences read rather than
   location-based guards, and recovery through the B+ tree. Engine suite
   under `BYTECASK_KEYDIR=blind`. G3, G4 measured.

Next, in order (Revision 2). Each lands with its tests, the engine suite on
all three trees, and before-and-after numbers on the buffer-pool rows:

4. **R1** — `lend_record`, one lookup per record read; `kd_read_value` from
   the same lease. G3 again.
5. **R3** — `FileStats::live_keys`, the range-delete estimate, vacuum's fast
   path on `live_keys`, the consistency check. All trees.
6. **R2** — the 12-byte entry. G1, G2, G3, G4.
7. **R4** — the 40-byte common node header. B+ tree unchanged in
   `map_bench` and `memory_profile`; G1 on the blind tree.
8. **R6** — the in-leaf search (#156). G2, G3; then pick the leaf size again.
9. **R5** — bulk-load slack; redistribution only if the random shapes still
   miss G1.
10. **R7** — recovery from sorted hint streams. `[model]` tests. G5.
11. **R8** — location-based guards; resolving before the commit group.

## Open questions

- **Leaf size.** 1,280 bytes (101 entries after R2, R4 and R6): at 2,560
  the index leaves ranges too long to scan.
- **12-byte entries.** Adopted as R2, with R1 for the read length and R3 for
  `del_range`'s live bytes.
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
