# Hint file compression — design

> **Status: built** (2026-09-26). Hint files are written zstd-framed, and
> every recovery path reads both layouts. The design sections below were
> first measured with a standalone harness over real hint files (§Harness
> measurements); §Measured in the engine has the before/after of the change
> itself.

## Problem

Recovery rebuilds the key directory from the hint files. With a warm page
cache that is CPU work, and it is what `ByteCaskDB/Recovery` measured until
this change: the benchmark only evicted the cache under `BC_DROP_CACHES=1`,
which needs root. After a reboot, or on a new node, the hint files come off the
device, and recovery is bound by how many bytes they hold.

10M keys, blind key directory, `max_file_bytes` 4 MiB, 1-byte values, SATA SSD
(Samsung 860 EVO), cache evicted with `posix_fadvise(POSIX_FADV_DONTNEED)`:

| Keys | Hint bytes | Warm, 16T | Cold, 16T | Cold phase 1 (scan + fences) |
|---|---|---|---|---|
| `seq` — `user::` + sequential UUID text | 675 MB | 0.26 s | 1.55 s | 1372 ms |
| `randtext` — `user::` + 16 random hex, random order | 475 MB | 0.31 s | 1.20–1.33 s | 1027 ms |
| `rand` — 16 random bytes | 395 MB | 0.28 s | 1.00 s | 801 ms |

Phase 1 reads at ~470 MB/s, the device's limit, and more threads do not help
(cold `seq` is 1.83 s at 4T, 1.55 s at 16T). The phases after it — splitters,
range merge, concat — cost the same warm or cold, because phase 1 has already
brought every file into the page cache.

The benchmark writes 1-byte values, which makes its hint files slightly larger
than its data files (675 MB against 644 MB for `seq`). A hint entry carries 23
fixed bytes and the key; a data entry carries 19 fixed bytes (15-byte header,
4-byte CRC), the key and the value. The hint is the smaller of the two once
values reach 4 bytes, and with realistic values it is a small fraction of the
data. What recovery reads is the hint bytes either way, which is what this
design shrinks.

## Goal

Cut the bytes recovery reads, without changing what a hint file says.

The target is a cold start: after a reboot, on a new node, or after uptime
long enough for the hint files — written once, read only at open — to leave
the page cache. Warm recovery is not a goal, and a change that speeds up a
cold start at its expense is acceptable.

- The entry encoding (`hint_entry.cppm`), the entry order — markers and range
  tombstones first, then the Put/Delete run sorted by key — and the recovery
  algorithms stay as they are. A decompressed hint file is byte for byte the
  file in the uncompressed layout, minus its trailer.
- File names stay `…_V01.hint`. Backups (the MariaDB plugin selects files by
  the `.hint` extension), `FileManifest::hint_path`, and replication file
  transfer are unaffected.
- Recovery's anonymous memory stays bounded, as `OpenForMerge` intends.
- No option. Hint files are always written compressed.

## Design

### File layout

```
[header: 16 bytes]
  magic        8 bytes   "BCHINTZ" + 0x81   (see Format detection)
  version      u8        1
  codec        u8        1 = zstd
  reserved     6 bytes   zero
[frame]*                 one zstd frame per ~16 KiB of hint entries
[trailer: u32]           ~CRC-32C over every preceding byte
```

- A frame holds a whole number of entries: the writer closes one once the
  entries buffered reach 16 KiB, so a frame holds less than 16 KiB plus one
  entry. A reader refuses a frame that claims more.
- zstd frames delimit themselves and carry their decompressed size (the writer
  compresses each frame in one call, so the size is always in the frame
  header). No frame index is stored: a reader walks frames with
  `ZSTD_findFrameCompressedSize` and sizes its buffer with
  `ZSTD_getFrameContentSize`.
- zstd level 1. Level 3 compressed no better on any key shape measured.
- The file-level CRC stays and covers the compressed bytes, so it is checked
  over 1.6–9× fewer bytes than before. zstd does not detect corruption on its
  own: flipping one random bit in a 16 KiB frame of real hint entries made
  decompression fail only 7–17% of the time, and the other 83–93% decoded to
  wrong bytes without error. One check is needed; two are not. zstd's frame
  checksum (`ZSTD_c_checksumFlag`: XXH64 of the decompressed bytes, truncated
  to 32 bits) catches the same flips, but it covers each frame on its own: a
  file missing its last frames, or holding one twice, passes every frame
  check and recovers without the keys those frames held. It also fails only
  while a frame is decoded, partway through recovery. The file CRC is verified when the file is opened, before any
  parsing, which is where `open_hint_or_rebuild` handles a damaged hint.
  Raw hints keep the same check, so there is one mechanism for both formats.

### Format detection

A hint file in the uncompressed layout starts with the `u64` sequence of its
first entry.
Sequences are assigned from 1 and never reach 2^63, so a first `u64` with the
top bit set cannot be a raw hint. The magic's last byte is `0x81`, which puts
that bit set in the little-endian `u64`. A file shorter than 8 bytes, or whose
first `u64` is below 2^63, is read as a raw hint.

The trailer is the bitwise complement of the CRC-32C, not the CRC itself. A
binary that predates this change reads a compressed hint as raw, finds its CRC
wrong, and does what it already does with a damaged hint: rebuilds it from its
data file (`open_hint_or_rebuild`). Downgrading costs one hint rebuild per
file and loses no keys. Upgrading needs nothing: raw hints stay readable, and
files sealed after the upgrade get compressed hints. Hint files are never
rewritten in place, so a database holds a mix of the two until vacuum turns
the old files over.

### Writing

`HintFile` in write mode buffers entries up to 16 KiB, then compresses the
buffer with one `ZSTD_compressCCtx` call, writes the frame and feeds it to the
running CRC. `close()` flushes the last partial frame and then writes the
trailer. `flush_hints_for` does not change: it still calls `append` and
`append_range_del` in the same order.

Writing is cheaper than before. Encoding took about 4 ms per 4 MiB data file,
single-threaded on the background hint worker, and it writes 1.6–9× fewer bytes.

### Reading: a scanner that decodes one frame at a time

The `Scanner` owns one frame buffer. `next()` decodes entries from it with the
existing `deserialize_entry`, and decompresses the next frame into the same
buffer when the current one runs out. Each thread holds one `ZSTD_DCtx`
(about 100 KB), shared by all the scanners it drives. The compressed file is
still read the way it was before: mapped by `OpenForMerge`, read in whole by
`OpenForRead`. Only frame buffers are anonymous memory.

A raw hint gets the same `Scanner` interface, backed by the whole mapping as
one "frame".

**The one contract change: key lifetime.** Before this change `HintEntry::key`
and `end_key` stayed valid for the life of the `HintFile`. With frames they stay
valid only until the scanner's next `next()` or `seek()` call, which may
overwrite the buffer. That rule is simpler and stricter than "until the frame changes", and
it holds for both formats. Every reader has to copy what it keeps past that
call:

| Reader | Holds a key past `next()` | Change |
|---|---|---|
| Radix — `recovery_build_from_hints` | No: inserts, and copies tombstones into `Key` | None |
| Keyed B+ tree — `recovery_build_sorted` | `cur` and `lookahead` across entries | Cursor owns its current key |
| Blind — `recovery_load_streams`, phase 1 | `prev_key`; fences hold a key span | Copy `prev_key`; a fence owns its key |
| Blind — `recovery_load_streams`, merge | `cur` and `lookahead` | Cursor owns its current key |

The cursor copies a key once per distinct key it passes, not once per entry.
Keys are at most `max_key_bytes` (4 KiB by default), so the copy is small next
to decompression.

The alternative — a scanner that keeps its previous frame alive, so a span
survives one frame change — was rejected. `load()` can read past several
frames of one key's duplicates while `best` still points into the first one.
It would need a lifetime rule that holds only as long as no key's duplicates
fill more than a frame. That is the kind of hazard the coding guidelines say
to design out, not document.

### Seeking and fences

The blind merge seeks each file to a fence, the last one below its range. A
scanner position is now a pair — the frame, as an offset into the file's
frames, and the entry's offset inside the decoded frame — and `seek()`
decodes that frame if it is not the one loaded. An uncompressed file is one
frame at 0, so its positions are the byte offsets they were.

Fences keep the rule they had — "a fence sits only on the first entry of a
key" — and the density: one on the first new key of every frame, and every
4 KiB inside a frame. A fence owns a copy of its key, since the frame it came
from is overwritten. The splitters come from the pooled fences as before, so
the ranges split the same way.

`data_start` (the position of the first Put/Delete) is a position like any
other.

### Memory

| Path | Frame buffers alive at once |
|---|---|
| Radix | one per worker |
| Keyed B+ tree | one per file (each file belongs to one worker) |
| Blind, phase 1 | one per worker |
| Blind, merge | ranges × files |

The blind merge is the one that grows: every range holds a cursor into every
file. The bound is ranges × files × 16 KiB, and it grows with the file count,
not the key count:

| Scenario | Files | Frame memory, 16 ranges | Key directory |
|---|---|---|---|
| Benchmark: 10M keys, 4 MiB files | 174 | 45 MB | ~140–190 MB |
| 100M keys, 64 MiB files, small values | ~110 | 28 MB | ~1.4–1.9 GB |
| 1B keys, 64 MiB files, small values | ~1,100 | 280 MB | ~14–19 GB |
| 1 TB of 4 KB values (250M keys) | ~16,000 | up to 4 GB | ~3.5–4.8 GB |

In the last row a cursor never holds more than its slice of a file (~50 KB),
but the bound is still of the order of the key directory. Today's mmap merge
has the same shape — the same cursors, each with 128–256 KiB of kernel
readahead — in the page cache, where it is reclaimable but hot. This change
moves that working set to the heap. It does not create it.

Bounding the merge at any file count is left as a follow-up (§Deferred): pick
the number of merge ranges from a memory budget,
`R = clamp(budget / (files × 16 KiB), 1, recovery_threads)`.

### Dependency

libzstd joins crc32c as a required dependency of the core engine, and follows
it into every build that links the engine. xmake builds zstd itself
(`add_requires("zstd", {system = false})`) instead of taking a system copy,
which is usually shared: every native binary links the same static archive,
and no shipped package gains a runtime `libzstd` dependency. Hosts need no
zstd package installed.

| Build | zstd change |
|---|---|
| Engine, tests, benchmarks (xmake) | `add_requires("zstd", {system = false})`; `add_packages("zstd")` beside every `crc32c` |
| Node, native N-API (`bytecaskdb_node` → `native/bytecask.node`, shipped in the npm package) | Comes with the xmake change; static, so the `.node` stays self-contained |
| Node, WASM (`wasm_embind`, `wasm_tests`, `wasm_smoke_test`, `wasm_engine_bench`, `wasm_memory_profile`) | `wasm/build.sh` cross-compiles zstd v1.5.7 into `wasm/build/zstd-wasm`, like crc32c; `add_wasm_sources` / `add_wasm_ldflags` in `xmake.lua` link it. CI's WASM dependency cache is keyed on `build.sh`, so it rebuilds on its own |
| Python wheels (`bytecaskdb_python`) | Comes with the xmake change; static |
| MariaDB plugin | `CMakeLists.txt` links the static archive xmake built under `~/.xmake/packages`, falling back to a system libzstd |
| CI, Dev Container, plugin `Dockerfile` | Nothing: xmake fetches and builds zstd, as it does crc32c on Ubuntu |

## Measured in the engine

`ByteCaskDB/Recovery`, cold (every file evicted with `posix_fadvise` before
each open), the benchmark's dataset — prefixed UUID text keys, 1-byte values,
4 MiB files — on the SATA SSD. Before and after binaries alternated on the
same disk; each figure is the mean of 5 opens:

| Keys | Threads | Before | After | |
|---:|---:|---:|---:|---:|
| 1M | 1 | 310 ms | 210 ms | 1.5× |
| 1M | 4 | 193 ms | 70 ms | 2.8× |
| 1M | 8 | 171 ms | 47 ms | 3.6× |
| 1M | 16 | 167 ms | 41 ms | 4.1× |
| 10M | 1 | 3.03 s | 2.24 s | 1.4× |
| 10M | 4 | 1.78 s | 0.63 s | 2.8× |
| 10M | 8 | 1.75 s | 0.39 s | 4.5× |
| 10M | 16 | 1.57 s | 0.33 s | 4.7× |

At 10M keys the hint files hold 675 MB of entries in 47 MB (14.2×, 16 KiB
frames). Before, recovery read at the SSD's limit and more threads barely
helped (1.78 s at 4, 1.57 s at 16); after, it scales with threads again. At
one thread it is now CPU-bound: 2.24 s of wall time, 2.11 s of it on the CPU.

These keys compress about as well as keys can. The projection below, from
the harness, is the better guide for keys with less in common.

## Harness measurements

A standalone harness read the real hint files of the databases below,
re-encoded them, wrote the result to disk, and timed phase 1's work — read,
decode every entry, record fences — with the cache warm and evicted. The merge
phases do not depend on the file encoding and are not part of these numbers.

Two sets of databases were measured:

- **1-byte values, 4 MiB files, 10M keys** — the recovery benchmark's own
  dataset (§Problem). Every hint entry has the same `value_size` and `seq`
  offsets advance at a fixed stride, which zstd removes almost entirely, so
  these ratios are an upper bound.
- **Values of 100–4096 bytes (uniform), 64 MiB files (the default), 2M keys** —
  the realistic set. Hint bytes per entry do not depend on the key count, so
  its ratios carry to 10M keys; its times are for 2M keys.

### Compression ratio

zstd-1, 64 KiB frames:

| Keys | 1-byte values: ratio · B/entry | 100–4096 B values: ratio · B/entry | Raw B/entry |
|---|---|---|---|
| `seq` | 14.5× · 4.6 | **9.1× · 7.3** | 66.1 |
| `randtext` | 2.90× · 16.0 | **2.41× · 19.3** | 46.5 |
| `rand` | 1.78× · 21.8 | **1.57× · 24.7** | 38.7 |

Realistic values cost about 3 B/entry: a varying `value_size`, and offsets
spread over a 64 MiB file. zstd level 3 compressed no better anywhere, and on
realistic `seq` worse (6.4×).

### Frame size

zstd-1, ratio, realistic values:

| Frame | `seq` | `randtext` | `rand` |
|---|---|---|---|
| 4 KiB | 7.3× | 2.30× | 1.55× |
| **16 KiB** | **8.5×** | **2.40×** | **1.57×** |
| 64 KiB | 9.1× | 2.41× | 1.57× |

16 KiB keeps 93–100% of the 64 KiB ratio at a quarter of the buffer. Decode
throughput on one core is 1.1–1.9 GB/s of raw hint bytes at 64 KiB, and about
a fifth lower at 16 KiB.

### Phase 1 on SATA

Cold, 16 threads, milliseconds:

| | `seq` | `randtext` | `rand` |
|---|---|---|---|
| raw, 1-byte values, 10M keys | 1293 | 945 | 770 |
| zstd-1, 1-byte values, 10M keys | 112 | 332 | 444 |
| raw, realistic values, 2M keys | 256 | 188 | 155 |
| zstd-1, realistic values, 2M keys | 37 | 81 | 104 |

4 threads read the same as 16: phase 1 is bound by the device before and
after. `rand` stays I/O-bound when compressed — 16 random bytes per key cannot
shrink, and nothing that keeps the keys can shrink them.

### Projection

Cold recovery, 10M keys, 16T, SATA, with realistic values: phase 1 scaled
from the 2M-key realistic run (×5 entries), plus the merge phases as measured
on the 10M-key databases, plus a second decode pass (§What it costs):

| Keys | Today | Projected | |
|---|---|---|---|
| `seq` | 1.55 s | ~0.41 s | 3.8× |
| `randtext` | 1.2–1.3 s | ~0.69 s | ~1.8× |
| `rand` | 1.00 s | ~0.78 s | 1.3× |

These are projections. The engine implementation has to measure them.

### What it costs

Both passes over a hint file — phase 1's scan and the merge — decompress it.
One decode pass over 10M keys costs, in CPU time, about 0.35 s for `seq`,
0.42 s for `randtext` and 0.26 s for `rand`, split across the recovery
threads. Warm recovery pays two passes on top of its time before the change:

| Warm, 10M keys | `seq` | `randtext` | `rand` |
|---|---|---|---|
| 16T: before → added | 0.26 s → +45 ms | 0.31 s → +52 ms | 0.28 s → +32 ms |
| 4T: before → added | 0.48 s → +175 ms | 0.63 s → +210 ms | 0.59 s → +130 ms |

That is +12–20% at 16 threads and +22–36% at 4, the default
`recovery_threads`, against a cold start 1.3–3.8× faster. Per §Goal, the warm
cost is accepted.

## Validation

1. **Benchmark first.** `ByteCaskDB/Recovery` measures a cold start: before
   each timed open it evicts the database's files with `posix_fadvise`
   instead of `drop_caches`, so it needs no root. It landed as its own commit,
   ahead of the codec, and gave the "before" column above.
2. **Hint file unit tests** (`tests/hint_file_test.cpp`): the header and the
   inverted trailer; round trips at frame sizes from 1 byte to the default;
   entries larger than a frame, up to a RangeDel with two maximal keys; an
   empty file; `seek()` back to every position a scan reported, across
   frames; the uncompressed layout read by both open modes; the inverted
   trailer failing a plain CRC check; a damaged frame failing the CRC; an
   unknown version refused; a frame header that passes the CRC but not zstd
   refused at decode.
3. **Model tests** (`[model]`): the existing ones run against compressed
   hints unchanged. A new one — hints split into many frames — writes
   8 KiB files cut into 64-byte frames, with hot keys rewritten until one
   key's duplicates span several frames, plus batches and range deletes, and
   recovers at 1, 2, 5 and 8 threads, then with every other hint rewritten
   in the uncompressed layout. All three key directories pass the suite.
4. **Sanitizers**: testing builds give every decoded frame a fresh
   allocation, so a reader that keeps an entry past its frame reads freed
   memory. Planting exactly that bug — the blind fence scan comparing against
   a stale key span — makes ASan report a heap-use-after-free in the new model
   test. The full suite passes under ASan, TSan and UBSan.

## Alternatives considered

- **A block index footer** (per-block first key, sequence bounds, range
  tombstones), so phase 1 would read footers instead of whole files. It would
  remove one of the two decode passes and let reading overlap the merge, but
  it changes what a hint file contains, not only how it is stored. On a cold
  start the gain is bounded by the merge phases and one decode pass
  (~0.25–0.3 s at 16T, more at 4T), and compression already takes the
  device-bound phase down 1.5–7×. Not worth the content change now; the
  next step if a cold start still needs to be faster.
- **Packed encoding** — front-coded keys and varints in place of the fixed
  23-byte header. It shrinks the files 1.5–5.7× with no dependency and decodes
  at 4–9 GB/s, but it is a new entry format, and zstd over the raw entries
  compresses better on every key shape measured.
- **zstd over the packed encoding**: no better than zstd over raw entries.
- **Whole-file decompression.** It keeps the old key lifetime, but it costs
  the raw hint size in anonymous memory for every file a merge holds open —
  675 MB for 10M `seq` keys, several times the key directory.
- **A shared cache of decoded frames**, with cursors holding copied keys. A
  k-way merge advances every cursor in turn, so any cache smaller than one
  frame per cursor misses on nearly every advance.
- **Checkpointing the key directory** instead of replaying hints: the fewest
  bytes possible (~12 per key), but it must stay consistent with vacuum,
  rotation and crashes. It is a separate design.

## Deferred

- Merge width from a memory budget, so the blind merge's frame memory stays
  bounded at any file count (§Memory).
- README: the published recovery figures are warm-cache figures. Replace them
  with cold-start figures from the new benchmark, and say which they are.
