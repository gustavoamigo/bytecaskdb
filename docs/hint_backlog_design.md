# Bounded hint backlog

Addresses [#146](https://github.com/gustavoamigo/bytecaskdb/issues/146).

## Problem

Each rotation queues one hint task on a single background worker. Nothing
limits that queue. Under sustained writes it grows. `~DB()` drains it
synchronously, so close time depends on how hard the DB was being written. A
SIGKILL during that drain moves the rest of the work into the next `open`,
which generates the missing hints serially before it can serve.

In the sysbench run from the issue, generating one hint took 13 s: the scan
does two unbuffered `pread`s and three heap allocations per entry, and on
virtiofs each `pread` costs about 22 µs. A new file arrived every 1.6 s, so
the queue grew about 8× faster than the worker could empty it.

The work is correct but has no time bound. The fix bounds it.

## Design

Three changes.

### 1. Buffered scan

Every sequential sweep (hint generation, vacuum, `create_manifest`, `resume`)
goes through `DataFileIterator`, which today calls the virtual
`DataFile::scan(offset)` once per entry.

- `DataFile` gets one new virtual, `read_raw(Offset, std::span<std::byte>)
  -> std::size_t`, which reads bytes at an offset and returns the count read.
  It never reads through the buffer pool; this is the same `Source::Bypass`
  rule as today, so a sweep does not flush the working set. The mmap back-end
  copies from the mapping.
- `DataFileIterator` owns a 1 MiB buffer. It frames entries out of that
  buffer and verifies each one with the existing `parse_header_and_verify`.
  When an entry crosses the end of the buffer, it refills starting at that
  entry. When a single entry is larger than the buffer, it grows the buffer
  to fit; the growth is bounded by `max_value_bytes`.
- The per-entry `DataEntry` in the iterator reuses its key and value vectors,
  so there is no allocation per entry.
- `DataFile::scan()` and its five implementations are removed. Their end
  checks (short header, `sequence == 0`, entry past the file end) move into
  the iterator unchanged. The active file's `io_data_file_scan` fault point
  moves into its `read_raw`. The buffer pool's `sweep_done()`, which dropped
  the whole file's page cache when a sweep ended, becomes a drop of each
  chunk's range right after it is copied out.
- `CommittedEntryIterator` also reuses its staged entries instead of copying
  each one into a fresh slot.

The result is about `file_size / 1 MiB` syscalls per file instead of
`2 × entries`. No format or API change.

Measured on a 64 MiB file with 300,937 entries (24-byte keys, 180-byte
values), in a release build on local disk with a warm page cache, over three
sweeps each:

| Back-end | Before | After |
|---|---:|---:|
| `Pread` | 359–384 ms | 40 ms (first sweep 149 ms) |
| `Mmap` | 48–85 ms | 38 ms |
| `BufferPool` | 376–779 ms | 47–53 ms |

Across the six `pread`-based sweeps, `strace -c` counted 3,611,247 `pread64`
calls before and 393 after. At the 22 µs per call measured on virtiofs in
the issue, the old count is the 13 s per file the issue reports; the new one
is bound by bandwidth.

### 2. Backpressure on rotation (on by default)

```cpp
struct Options {
  // Maximum number of sealed files waiting for a hint. A rotation that would
  // exceed it waits for the worker. 0 turns backpressure off.
  std::uint32_t max_hint_backlog{4};
};
```

- `BackgroundWorker` gains `pending()`, the number of queued tasks plus the
  running one, and `wait_pending_below(n)`.
- Both places that seal a file, `rotate_active_file` and `resume`, call
  `wait_for_hint_backlog()` before they seal, then queue the task through
  `dispatch_hint()`. With backpressure on, the wait blocks until fewer than
  `max_hint_backlog` tasks are pending. It runs on the writer with the write
  path held, so writes stall; reads do not. Waiting before sealing, rather
  than before queuing, means a stalled writer has not yet created a sealed
  file without a queued task.
- This cannot deadlock because hint tasks never take the write mutex, since
  `flush_hints_for` is static. That becomes a stated invariant of the worker.
- A hint task that throws still counts as finished, so a failed hint releases
  the stall instead of wedging writes.

With backpressure on, the backlog never exceeds `max_hint_backlog`, which
bounds both paths from the issue:

- **Close** writes at most `max_hint_backlog` hints: the backlog it drains.
  (The active file's hint is written by the next open, as today.)
- **Open after a SIGKILL** finds at most `max_hint_backlog + 1` data files
  without a hint: the backlog plus the active file.

Each bound is a number of files times the scan time of one file, and step 1
makes that scan time small. A close deadline is not needed: it would only
move the work into the next open.

**Opting out.** With `max_hint_backlog = 0`, rotation never waits, and close
and open are unbounded again, as they are today. This suits bulk loads where
write throughput matters more than restart time.

A stall is visible latency, which principle 3 argues against. It is the right
trade here because the alternative is a stall later, at close or open, that
an operator's supervisor turns into a kill. With a fast scan, a stall only
happens when the disk cannot keep up with reading back what was just written.

### 3. Backlog in `stats()`

- `bytecask.hint_backlog` (gauge): `worker_.pending()`.
- `bytecask.hint_backpressure_stalls` (counter): the number of rotations that
  waited.
- `bytecask.hint_backpressure_stall_us` (counter): total time spent waiting.

A backlog near the limit, or a stall counter that keeps rising, signals the
problem before it becomes a stall or a kill.

In the WASM (`BYTECASK_SINGLE_THREADED`) build the worker runs tasks inline,
so `pending()` is always 0 and backpressure never waits.

## Out of scope

- **Parallel hint generation at open or close.** With the backlog bounded,
  this is at most a constant-factor gain on a few files.
- **Building hints in memory while writing**, which would avoid re-reading the
  file at rotation. It is possible, but costs memory for one file's keys, and
  step 1 may make it unnecessary. Revisit only if measurements say so.

## Tests

- Scan: the existing model-based recovery tests and scan tests cover the new
  iterator. New cases cover an entry that straddles the buffer boundary, an
  entry larger than the buffer, a zero-filled tail, and a truncated last entry
  on each back-end (`Pread`, `Mmap`, `BufferPool`, active file).
- Backpressure (`[hint_backlog]`): a test holds hint tasks behind a test hook
  (`test_before_hint_`) and writes with `max_file_bytes = 1`, so every put
  rotates. It asserts the writer stalls with `hint_backlog` at exactly the
  limit, and that it finishes and the stall counters move once the worker
  is released. With the option set to 0, every put returns while the worker
  is held and the backlog grows past any limit.
- Bound: while the writer is stalled, the test copies the directory, as a
  kill would leave it. It asserts that at most `max_hint_backlog + 1` data
  files lack a hint, and that opening the copy recovers every acknowledged
  put.
- `stats()`: the new keys are in the expected key list, and the tests above
  check that `hint_backlog` rises and falls.

## Measurement

The sweep numbers are above.

`engine_bench` was run on `main` (694b568) and on this change, built with
`BYTECASK_NO_ROCKSDB=1`, at 50k keys: 5 rounds of each binary, alternated
so both see the same machine state, on a 4-vCPU VM. It was run twice:

- With the default 64 MiB files. The dataset is ~14.5 MB, so no timed loop
  rotates a file, and neither the sweep nor backpressure runs in them.
  This is the check that nothing else moved.
- With `BC_MAX_FILE_BYTES=1MiB`, so each timed write loop rotates about 14
  times and hint sweeps and backpressure run alongside the writes.

No row moved beyond its run-to-run spread in either run. The medians
differ by -20% to +38%, in both directions, but a single row's five runs
vary by 10–90% on this machine (widest on `Sync` rows, where `fdatasync`
dominates). The two runs do not agree on which rows are faster or slower,
and the timed read loops (`Get`, `GetMT`), which never call the changed
code, vary as much as the write loops. `Recovery` is the one family that
came out lower in every thread count in both runs, by 0–6%, inside its
2–28% spread. Its timed opens read hint files that already exist. Apart
from the very first one, which sweeps the setup's last active file (the
new scan makes that faster), the only data file an open sweeps is the
empty one the previous open left, where both versions stop at once. So the
change is not expected to move it.

The sysbench `oltp_read_write` scenario from the issue has not been re-run.
