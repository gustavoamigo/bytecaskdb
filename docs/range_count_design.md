# Counting keys in a range without reading records

Status: implemented (`Snapshot::count_keys`). It implements exact counting up to a limit;
estimating the rest of a range beyond the limit is a follow-up (see the end).

## Problem

The MariaDB optimizer asks the storage engine how many rows fall in a key
range (`handler::records_in_range`) before it runs a query, and a stored
procedure asks again on every call. The plugin answers exactly, up to 1,024
keys, by walking the range with an iterator and counting
(`ha_bytecaskdb::records_in_range`). Its comment says why that was cheap:
"key enumeration is an in-memory tree walk". That was true of the radix and
keyed B+ tree key directories. It is not true of the blind-leaf tree, the
default since #160: a leaf holds no key bytes, so every step of the walk
reads the key's record to learn the key. Primary-key ranges are also walked
with `iter_from`, which reads each row's value too.

Measured on HammerDB TPROC-C through the MariaDB plugin — 70 warehouses,
14 virtual users on disjoint warehouses, `--profile=fast`
(`bytecaskdb_sync = AT_INTERVAL`, 16 GiB buffer pool), SATA SSD, 16-thread
Ryzen 7 3700X:

| | Result |
|---|---|
| NOPM | 144,184 (143,001 in a repeat run) |
| mariadbd CPU | 11.7 of 16 hardware threads; the run is CPU-bound |
| Buffer pool reads per commit | ~2,900 |
| `records_in_range`, inclusive CPU share | 41.7% of mariadbd |
| Query execution in the handler (`index_read_map`, `write_row`, `update_row`, commit) | under 10% combined |

Most record reads are the optimizer counting rows, not queries fetching them.
Ranges of TPC-C size make this expensive: Delivery looks for the oldest of
the ~900 `new_order` rows in each district, so each estimate walks up to
the cap.

Lowering the cap to 8 as an experiment shows what is at stake:

| `kRangeCountCap` | NOPM | Pool reads / commit | mariadbd CPU |
|---:|---:|---:|---:|
| 1,024 (current) | 144,184 | 2,909 | 1,170% |
| 8 (experiment) | 164,495 (+14%) | 672 (−77%) | 570% (−51%) |

InnoDB on the same setup (`innodb-fast.cnf`: `flush_log_at_trx_commit = 2`,
16 GiB buffer pool) reaches 179,595 NOPM, writing 16.8 GiB in the measured
3 minutes to ByteCaskDB's 3.4 GiB. ByteCaskDB is at 80% of InnoDB's
throughput today and at 92% with the counting cost mostly removed.

A small cap is not the fix: it throws away the exactness that makes the
estimates useful, and still reads a record per counted key. The fix is to
count without reading records.

## Proposal

### Engine: `Snapshot::count_keys`

```cpp
// Live keys in [from, to), counted no further than `limit`: returns
// min(count, limit). Throws std::invalid_argument if from >= to; the plugin's
// count_range answers lo >= hi with 0 before calling it.
[[nodiscard]] auto count_keys(BytesView from, BytesView to,
                              std::size_t limit) const -> std::size_t;
```

On `Snapshot` only, so the count sees exactly what a read at that snapshot
sees. The plugin estimates for a transaction, which reads at its snapshot;
nothing needs a count of the latest committed state, so `DB` gets no
`count_keys`.

Requirements:

- **Exact at the edges.** `from` is inclusive and `to` exclusive, including
  when either equals an existing key, falls between two leaves, or lies
  beyond either end of the key directory.
- **No slower than today on small ranges.** Many optimizer ranges hold one
  key or none — equality on a primary or unique key — and cost the current
  walk two or more record reads: one to position, then one per key it steps
  onto, including the first key at or past the end. `count_keys` positions
  both ends, at most two reads, whatever the range holds. (Positioning in a
  blind leaf reads the crit-bit candidate, which need not be the entry it
  lands on, so checking the first key against `to` instead would cost the
  same second read.)

A count is two positions and the entries between them. Positions come from
`lower_bound`, which already exists for every key directory; the entries
between them are counted from node sizes, never from keys.

| Key directory | Positioning | Counting | Record reads |
|---|---|---|---|
| Blind-leaf B+ tree (default) | `lower_bound(from)` and `lower_bound(to)`: one read each, to place the target within its leaf | walk leaves from the first position to the second, adding each leaf's `count` | **≤ 2, whatever the range** |
| Keyed B+ tree | `lower_bound` on stored keys | the same leaf walk | 0 |
| Radix tree | `lower_bound` on stored keys | iterate keys in memory up to `limit` | 0 |

For the B+ trees the walk is O(depth + leaves spanned), stopping as soon as
the running total reaches `limit`. Leaves hold about 80 entries at
1,024 bytes, so a 900-key range spans about 12 leaves. Both trees' iterators
keep a stack of `(node, index)` frames over the same node type, so one
function counts from one stack to the other — the rest of the first leaf,
then each following leaf's `count`, then the part of the last leaf before
the end — and each iterator exposes it as `count_until(end, limit)`. No new
node state.

### Plugin: `records_in_range`

`count_keys` on the transaction's snapshot, plus the transaction's own
buffered writes in the range, which the current walk merges in:

- For each key in `lookup_` (the transaction's ordered overlay) within
  `[lo, hi)`: a buffered put of a key the snapshot does not hold adds one; a
  buffered delete of a key it does hold subtracts one. Each check is one
  `contains_key` on the snapshot — the overlay holds a transaction's own
  writes, a handful in TPC-C.
- The adjustment is bounded by the same cap. If the snapshot's count
  already reached it, the range counts as capped and the buffered writes are
  not checked; otherwise the adjustment stops once the running count reaches
  the cap. A transaction that has buffered many rows in one range pays at
  most the cap in existence checks.
- The cap and the fallback stay as they are: exact up to 1,024, and
  `max(1,024, rows / 10)` above it.
- **Never 0.** MariaDB takes 0 as "this range is certainly empty" and may
  skip reading it, which would drop rows from a result. A range counted as
  empty still returns 1, as today. Every other value is an estimate that
  affects only the plan.

The estimate stays exact where it is exact today, so
`tests/functional/test_records_in_range.py`, which checks exact counts
including rows buffered in the current transaction, keeps passing
unchanged.

## Alternatives considered

- **Lower the cap.** Measured above: a gain, but estimates degrade and each
  counted key still costs a read.
- **Raise the cap, now that counting is cheap.** Makes more ranges exact,
  but the walk stays linear in the range, and above any cap the answer is
  still a fixed fraction of the table. The follow-up's estimate removes that
  step for every range size at O(depth) cost.
- **Walk primary-key ranges with `keys_from` instead of `iter_from`.** Avoids
  copying values; still one record read per key.
- **Subtree counts in inner nodes** (an order-statistics tree). Counts any
  range in O(log n), but every insert and delete updates a count in each
  node on its path, and every path copy carries it. The cap already bounds
  the leaf walk, so the extra write-path cost buys nothing here.

## Tests

Engine (`tests/bytecask_test.cpp`), run under all three key directories in
CI:

- `count_keys` equals a `keys_from` walk over random ranges of a random key
  set, including bounds equal to existing keys, bounds that fall between two
  leaves, empty and reversed ranges, ranges beyond either end, and `limit`
  below, at and above the true count.
- Record reads: at most two per count on the blind tree, whatever the range
  holds, and for 0-, 1- and 2-key ranges no more than the equivalent
  `keys_from` walk.
- Ranges spanning many leaves (enough keys to split several levels) and
  ranges within a single leaf.
- Snapshot isolation: a count on an old snapshot is unchanged by later puts,
  deletes and `del_range`.
- The blind tree reads at most two records per count: the
  `bytecask.disk_reads` counter, or a test resolver that counts `key_at`
  calls.

Plugin: `test_records_in_range.py` unchanged, plus cases where
- the transaction's buffered writes put and delete keys inside the counted
  range, both of keys the snapshot holds and of keys it does not;
- the transaction deletes every row of a non-empty range: the estimate is
  1, never 0;
- the transaction buffers more rows in one range than the cap: the count
  stops at the cap without checking every buffered key.

## Measurement

- `engine_bench` before and after (the repository's rule for any performance
  change), plus a new `CountRange` case at 0, 1, 10, 1,000 keys and the
  cap, comparing `count_keys` with the `keys_from` walk it replaces.
- The HammerDB probe above, before and after: NOPM, pool reads per commit,
  mariadbd CPU, and a CPU profile confirming `records_in_range` drops out of
  the top.

## Results

`engine_bench`, `CountRange` (50,000 keys, buffer pool): counting a range
with `count_keys` against the `keys_from` walk it replaces, up to the
plugin's 1,024 cap.

| Keys in range | `count_keys` | `keys_from` walk | Speed-up |
|---:|---:|---:|---:|
| 0 | 598 ns | 600 ns | 1.0× |
| 1 | 582 ns | 717 ns | 1.2× |
| 10 | 587 ns | 1,741 ns | 3.0× |
| 1,000 | 637 ns | 108,300 ns | 170× |
| 1,024 | 642 ns | 112,446 ns | 175× |

The rest of `engine_bench` before and after: reads, single-threaded writes,
scans and recovery within ±5%. The multi-threaded synced-write cases vary
between 0.44× and 2.01× in both directions from run to run; they are bound by
`fdatasync` on the benchmark disk, and this change adds functions without
touching the write path.

HammerDB TPROC-C, the setup in the Problem section:

| | NOPM | Pool reads / commit | mariadbd CPU | `records_in_range` CPU share |
|---|---:|---:|---:|---:|
| Before | 144,184 | 2,909 | 1,170% | 41.7% |
| Cap-8 experiment | 164,495 | 672 | 570% | — |
| **`count_keys`** | **201,128** (+39%) | **129** | 990% | 2.2% |
| InnoDB, same setup | 179,595 | — | — | — |

ByteCaskDB goes from 80% of InnoDB's NOPM to 112%. It beats the cap-8
experiment because that still read up to eight records per estimate, values
included, where `count_keys` reads at most two.

## Scope

This removes the counting cost, at most the 42% of mariadbd's CPU that
`records_in_range` takes today. Most of the rest is MariaDB's own SQL and
stored-procedure work. In the cap-8 experiment mariadbd fell to 570% CPU
while NOPM rose only 14%, and the engine's commit-wait counters did not
move, so the next limit is probably not in the engine's commit path. An
off-CPU profile after this change would show what it is.

On the keyed key directories (`BYTECASK_KEYDIR=btree`, `radix`), which never
read records to walk, the gain is small; they get `count_keys` so every build
has the same API and passes the same tests.

## Open questions

None at this point.

## Follow-up: estimating beyond the limit

Not part of this change; tracked as a GitHub issue. Today a range holding
more than the limit gets a fixed tenth of the table from the plugin. When
the walk reaches `limit` before the end of the range, it could instead
estimate what is left without walking it. Everything the estimate needs
is already in hand:

- **Where the walk stopped:** its iterator's path, a `(node, index)` frame
  per level from the root.
- **Where the range ends:** the path of `lower_bound(to)` — the one extra
  record read the blind tree pays, which a range this size would pay anyway.
- **How full leaves are here:** the walk counted `limit` keys across some
  number of leaves, a measured entries-per-leaf for this part of the tree.

At each level where the two paths differ, the subtrees strictly between
them are counted from the index in each frame. Each is taken to hold as many
leaves as the fanout of the nodes on the two paths — their `count`s, already
in memory — implies for its height. Leaves remaining, times the measured
entries per leaf, is the estimate of the rest; the estimate of the range is
`limit` plus it. The API would report whether a number is exact or
estimated.

- **Cost:** O(depth) arithmetic on nodes already visited, after the one
  positioning read. Nothing more is walked, whatever the range's size.
- **Accuracy:** exact up to `limit`. Beyond it, node fill in a B+ tree
  varies between half and full at each level, and a subtree's size is
  estimated from the fanout of every level below it, so in the worst case
  errors compound with depth — up to 2× per level spanned. How close it is
  in practice is for the tests below to show; they hold it to 2× over
  uniform and skewed key sets. Even the worst case keeps
  the estimate proportional to the range, which is what the optimizer needs
  to rank a range of a few thousand rows against a full scan, and what a
  fixed fraction of the table cannot give.
- The keyed B+ tree estimates the same way from its own nodes. The radix
  tree has no per-node counts and would keep reporting only "at least
  `limit`".
- Tests: over uniformly and skew-distributed key sets, ranges of 2×, 10×
  and 1,000× `limit` keys estimated within 2× of the true count (a target,
  not a bound to loosen), a larger range never estimated below a range it
  contains, and at most one record read beyond counting `limit` keys. In the
  plugin, a ~20,000-row range estimated in proportion rather than as a
  tenth of the table.
- HammerDB's ranges are all under the limit, so this changes plan quality
  for workloads with wide ranges, not TPC-C throughput.

