# Chaos Testing — Design

> Status: first version implemented (`tests/chaos/`, `chaos-nightly.yml`).
> Tracks issue #230; the Elle workload runs on chaosfs too (#232,
> `isolation_checking_design.md`, *Chaos*). The cgroup memory hazard (#269)
> is still to come.

## Why

Every black-box check today ends a run with one hazard: SIGKILL
(`tests/crash/crash_consistency.cpp`, the Elle kill mode). SIGKILL leaves
the page cache intact, so everything the process wrote survives it, synced
or not. The engine's fault injector (`[prove_*]`) covers I/O errors, but
from inside, one syscall at a time, at checkpoints the engine itself
declares. Nothing runs the engine as a user would meet it: a process on a
disk that fills up, stalls, returns `EIO`, loses power, or on a machine
that runs out of memory, with those events landing wherever they land.

`correctness_validation.md` lists what that leaves out: power loss, torn
sectors, directory entries that are not durable, and the reverts the crash
harness cannot catch (a sync removed before degrading, a hint written in
place). This design closes those gaps with one rig, run every night.

## Trust boundary

The engine is correct only on a filesystem that keeps the POSIX contract.
The rig tests the engine against every behaviour that contract, and Linux's
documented semantics, allow. It does not test a filesystem that breaks
them.

In scope — a correct filesystem can do all of this:

- Any syscall returns an error it is documented to return: `EIO`,
  `ENOSPC`, `EDQUOT`, `EROFS`, `EINTR`, `EMFILE`, `ENOMEM`.
- `write` returns a short count.
- Any operation takes arbitrarily long.
- On power loss, data and metadata not made durable by `fsync`/`fdatasync`
  (and, for a directory entry, by an `fsync` of the directory) are lost,
  kept, or kept in part, at sector granularity. Unwritten ranges read back
  as zeros.
- After `fdatasync` fails, the dirty pages it covered are marked clean and
  may never reach the disk; a later `fdatasync` on the same file can
  succeed without writing them. Reads keep returning those bytes until the
  pages are evicted, then return what is on disk. This is Linux's behaviour
  since 4.13 (`errseq_t`), not a bug.

Out of scope:

- `fsync` that returns success without making data durable, including a
  success after an error consumed through another descriptor (the case
  `CONTRACT.md` names as the `fdatasync` trust assumption).
- Reordering across a successful `fsync`, reads returning bytes never
  written, bit rot at rest. Corruption of acknowledged data is covered by
  `[prove_corruption]`, where the contract is fail-stop.

## Invariants

The same invariants hold whether the engine recovers online, through
`resume()`, or by a restart and `DB::open`. The rig checks them after every
recovery of either kind.

| # | Invariant |
|---|---|
| I1 | **Durability.** Every write at or below the durable watermark — the highest `durable_sequence()` observed and every `CommitResult` with `durable == true` — is present after any hazard, power loss included. |
| I2 | **Prefix.** The recovered contents equal the model after some prefix of the commit order. A batch is one operation. No value that was never written; no older value where a newer one is required. |
| I3 | **Honest failure.** A call that returns reported what happened. A write that threw is either applied in full or not at all, and I2 places it at its position in the order. A read never returns a value outside the model's candidates. |
| I4 | **Degraded is readable.** While degraded, reads succeed (bar their own I/O errors) and satisfy I2 and I3; writes throw `DbDegraded`. |
| I5 | **Recoverable.** Once the hazard lifts, `resume()` succeeds, or a restart's `DB::open` with default options succeeds, within a time bound. No state the rig can produce needs a manual repair. A hazard still in force may make `open` or `resume()` fail, but not change the data on disk in a way I1–I2 forbid. |
| I6 | **Resume equals restart.** The contents `resume()` publishes are the contents a restart would recover from the same disk, and both survive a later power loss up to the watermark. |
| I7 | **Recovery agrees with itself.** Serial and parallel recovery give the same contents, `file_stats`, key count and `durable_sequence`, as `[model]` checks. |
| I8 | **No undefined behaviour.** No sanitizer report, no signal the rig did not send, no abort the policy below does not allow. |
| I9 | **Liveness.** No hang: after a stall ends, writes complete, and the number of sealed files waiting for hints stays within `max_hint_backlog + 1`. |

An abort is a failure except `std::terminate` from `std::bad_alloc` under
the allocation-failure hazard; the restart that follows must still satisfy
I1–I7.

## Hazard axes

Each run draws from four independent axes. Randomness does the rest: the
rig does not aim a fault at a line of code; it makes each fault frequent
enough, and the workload busy enough, that faults land in every window
over many runs. The proof matrix stays the place for aimed faults.

### 1. How a process life ends

| Terminator | Mechanism | What survives |
|---|---|---|
| Clean close | worker returns from `main` | everything |
| SIGKILL | orchestrator | the page cache (all written bytes) |
| Power loss | chaosfs freezes, SIGKILL, then drops non-durable state | only what was made durable, plus a random part of the rest |
| OOM kill | cgroup `memory.max` below the working set | the page cache |

Every terminator can land during `DB::open`, `resume()`, vacuum, rotation,
hint writing and close, not only during steady writes.

The first version of this idea was to trim or damage the active file after
a SIGKILL. Without knowing what was synced, a trim can cut acknowledged
durable data and report a bug that is not one, or stay above the synced
point and test nothing. chaosfs knows exactly what is durable, so power
loss is modelled there instead.

### 2. I/O faults

Injected by chaosfs per operation, with probabilities drawn per window.

| Fault | Operations | Notes |
|---|---|---|
| `EIO` | `read`, `write`, `fsync`, `fsyncdir`, `create`, `rename`, `unlink`, `truncate` | `fsync` `EIO` follows the Linux semantics above |
| `ENOSPC` / `EDQUOT` | anything that allocates | driven by a capacity, not a coin; see below |
| `EROFS` | every mutating operation, until cleared | `errors=remount-ro` |
| Short `write` | `write` | a prefix of the buffer lands |
| `O_DIRECT` refused | `open` | exercises the buffer pool's per-file fallback |

**Disk full** is a capacity: chaosfs counts allocated blocks and refuses
allocation past it. The capacity shrinks and grows during a run, as other
tenants of a disk would make it. Allocation follows one of two models,
drawn per run: *extent*, where overwriting allocated blocks never fails
(ext4, XFS), and *copy-on-write*, where any write can fail (btrfs, ZFS).
With the extent model most `ENOSPC` lands on the zero-fill ahead of the
active file, not on a record; with copy-on-write it lands anywhere.

### 3. Resources

| Hazard | Mechanism | Surfaces as |
|---|---|---|
| Memory pressure | cgroup v2 `memory.max` | OOM kill (terminator above) and page-cache reclaim |
| Allocation failure | `RLIMIT_AS` | `std::bad_alloc` anywhere, `ENOMEM` from `mmap` |
| Descriptor exhaustion | `RLIMIT_NOFILE` near the open-file count | `EMFILE` at rotation, vacuum, open |

`RLIMIT_AS` cannot run under ASan, which reserves terabytes of shadow
memory; that hazard runs in the release leg only.

### 4. Timing

chaosfs adds latency per operation from a heavy-tailed distribution, and
stalls: every operation, or only `fsync`, blocks for up to several seconds.
Slow I/O is a hazard in its own right (I9) and widens every window the
other axes aim at: a kill during a 2 s `fsync` lands inside group commit.

## Architecture

```
run_chaos.py (orchestrator, seed)
 ├── chaosfs.py          FUSE mount; in-memory POSIX model; faults; control socket
 ├── chaos_worker run    engine + workload, in a cgroup, under rlimits,
 │                       on the chaosfs mount; history frames on fd 3
 └── chaos_worker check  reopens a copy of the post-crash image; checks I1–I7
                         against the whole history of the episode
```

### chaosfs

A FUSE filesystem in Python that holds the whole tree in memory. Every
inode keeps two images: *volatile*, what reads return, and *durable*, what
survives power loss. Directories keep volatile and durable entry sets the
same way.

- `write`, `truncate`, `fallocate` change the volatile image and mark the
  touched sectors dirty.
- `fsync` / `fdatasync` copy the file's dirty sectors and size to the
  durable image. On an injected `EIO` they mark those sectors clean instead,
  without copying; reads still return the volatile bytes until an eviction
  event reverts them to the durable ones.
- `fsync` on a directory makes its entry changes durable.
- `create`, `rename`, `unlink` change the volatile entry set and append to
  a per-directory log of pending changes.

**Power loss**: freeze all operations, SIGKILL the worker, unmount. Then,
for each file, each dirty sector either reaches the durable image or not,
and the size is either the durable one or the volatile one. For each
directory, a prefix of its pending changes is applied — what a journaling
filesystem does. In a quarter of crashes, a random subset is applied
instead, which POSIX also allows. The result is the new tree; remount it.
Both images and the choices made are saved, so a failure replays from the
image exactly.

The kernel caches FUSE pages. chaosfs mounts without `writeback_cache`, so
every write reaches it synchronously, and invalidates a file's cached pages
(`notify_inval_inode`) on eviction and on remount. Shared read-only `mmap`
works through the page cache as on any filesystem.

chaosfs is a test oracle, so it is tested as one: a property test runs
random operation sequences against it and a reference model of the rules
above, and a passthrough run with no faults passes the existing
`crash_consistency` harness.

Python keeps it small and readable. It will be slow — FUSE round trips
cost tens of microseconds — which only matters as a throughput limit on the
workload; slower I/O is itself one of the hazards.

### chaos_worker

A C++ binary with two modes, built on the public API only.

`run` opens the database with options drawn from the seed (backend,
`max_file_bytes`, `recovery_threads`, `max_hint_backlog`, sync ratio,
vacuum on or off) and runs the `crash_consistency` workload — its
operation model, value encoding and Intent/Commit/Abort frames are factored
out of that harness and shared. Added to it:

- **Reader threads** check every value they see against the candidates
  frame by frame (I3, I4).
- **Recovery loop.** On `DbDegraded` or an I/O error, the writer calls
  `resume()` with backoff. After a success it scans the whole database
  and emits a `View` frame; the checker holds that view to I2 and later
  to I6.
- A `Throw` frame for a write that threw: its outcome is unknown, and the
  checker treats it as optional at its position.

History frames go to fd 3, a pipe to the orchestrator, which writes them
outside the mount.

`check` reads the episode's history, opens copies of the post-crash image
with serial and parallel recovery, and checks I1, I2, I5–I7. A thrown write
is unknown, so the prefix search allows each one either way; the engine
degrades at the first, so a life holds few.

### Episodes

An episode is a seed, a fresh mount and 20–50 process lives on it. Each
life draws its worker options and its hazard timeline: fault windows of
each axis, opening and closing at random times, and a terminator. Between
lives the orchestrator runs `check` on the post-terminator image; the next
life opens that same directory, so every life after the first starts from
a crash and recovery is itself under fault.

The orchestrator enforces I9: once the hazards are lifted, the worker must
make progress within a bound. If it does not, the orchestrator saves the
worker's stacks (`eu-stack -p`) and fails the episode.

## Reproducing a failure

A seed fixes each life's options, timeline and every chaosfs decision by
operation count, not the thread interleaving, so it replays a failure's
shape, as `crash_consistency --seed` does today. What replays exactly is
the saved crash image: `chaos_worker check --dir <image> --history <file>
--state <file> --seed <life seed> --work <dir>` reruns recovery on the bytes
that failed. Each failure keeps the seed, the
history, the chaosfs operation and fault log, and the pre- and post-crash
images.

## Proving the rig

Like the soak, the rig earns its cost only if it catches what nothing else
does. `tests/chaos_mutations/` holds mutations of the engine, each with an
`Expected: caught | NOT caught` header, and `scripts/chaos_mutation_check.sh
[minutes] [patch...]` applies each, rebuilds `chaos_worker`, runs the rig on
two seeds with a budget per seed, and fails if a mutation expected to be
caught survives (#268). A patch can carry `Budget: N` minutes and `Disable:
a,b`, the hazards to leave out so the run concentrates on the ones that reach
it; with read faults, evictions and the descriptor limit all left out, the
worker's readers also count a read's I/O error as a violation
(`--strict-reads`), since nothing in the run can fail a read.

Most mutations revert a fix the rig prompted, so the set also guards those
fixes:

| Mutation | Reverts | Result |
|---|---|---|
| `commit_skips_fdatasync`: the commit flush skips its `fdatasync` and still reports durable | — | caught in ~20 s, every run |
| `no_dir_sync_new_data_file`: a new data file's name is not synced | #199 | caught in 30–90 s, every run |
| `resume_trusts_page_cache`: `resume()` publishes the tail without rewriting it | #240 (#231) | caught in 1–2 min, every run |
| `open_trusts_page_cache`: `open` indexes a hint-less file without making it durable | #240 (#231) | caught in 0.5–3 min, every run |
| `close_swallows_error`: `close()` drops its final `fdatasync`'s error | #260 (#257) | caught in 0.5–3 min, every run (every life a clean close) |
| `hint_read_error_kills_process`: a hint read error kills the process | #255 (#237) | caught in 2–5 min, every run |
| `vacuum_drops_before_durable`: vacuum drops records superseded by non-durable writes | #261 (#245) | caught in 2 of 5 runs of 10 min: rare |
| `hint_written_in_place`: hints written in place, not renamed | — | caught twice in ~45 min: rare |
| `no_sync_before_degrade`: a failed append degrades without syncing | — | not caught: #240's rewrite in `resume()` covers it |
| `truncate_lowers_end_after`: the logical end drops after `ftruncate`, not before | #248 (#236) | not caught: the stale end lasts one `resume()` retry; #248's `prove_resume` cell guards it |

"Rare" (`Expected: caught (rare)`) marks a mutation the rig catches but not
within a fixed budget: the script runs and reports it, and a survival is not a
failure.

What building the set taught:

- **Two of the design's mutations were redundant in today's engine.** "No
  `fdatasync` before sealing at rotation" is covered by `shrink_to_fit()`'s
  own `fdatasync` before anything is published, and "no sync before degrading"
  by #240's rewrite-then-sync in `resume()`. The first was replaced by the
  commit flush skipping its `fdatasync`; the second is kept as not caught.
- **A partial revert can be masked.** Removing only `open`'s rewrite left the
  durable trim, whose `fdatasync` covers the file; only the full revert of
  #240's `open` half is caught.
- **The rename of a hint is load-bearing.** The design expected a hint
  written in place to survive, torn hints being rebuilt. It is caught: the
  rebuild of a hint that fails its CRC skips a hint-less file's tail
  handling, and only the rename guarantees a hint exists only once complete.
- **Rare paths need focus, and one needed the rig to change.** A `close()`
  that swallows its error shows only after a failed writeback, an eviction
  and a clean close, about one life in 300; its run ends every life in a
  clean close. The truncate revert (#248) needs an eviction between a failed
  `fdatasync` and the next `resume()`, which the timeline almost never
  produced; chaosfs now evicts a failed `fdatasync`'s lost pages at once with
  some probability (`evict_failed`), as memory pressure can. Even so that
  mutation is not provable here, and #248's unit test guards it.
- **A fixed budget does not suit every mutation.** Vacuum's and the in-place
  hint's need a coincidence the random timeline reaches a few times an hour;
  they are kept, marked rare, and run without failing the check.

## CI

`chaos-nightly.yml`, daily, release and ASan legs, 40 minutes each, run id
as seed. The container runs with `--device /dev/fuse --cap-add SYS_ADMIN`. `crash-nightly.yml` stays: it
is fast, needs no FUSE, and covers kills on the real page cache.

## Implementation notes

Where the first version differs from the design above, and why.

- **No `Mmap` backend.** chaosfs mounts with FUSE `direct_io` so that every
  read and write reaches the model, which then is the page cache. The kernel
  refuses a shared mapping of such a file, so the worker draws only `Pread`
  and `BufferPool`. Private mappings (hint files) work.
- **Power loss fences the mount.** A SIGKILLed process cannot exit while
  one of its FUSE requests is held in the daemon. So the orchestrator
  freezes the filesystem, sends SIGKILL, applies the crash, and then fails
  every request until the process is gone. A request that arrived before the
  crash never applies after it, even when a stall or the freeze held it.
- **Every unknown outcome is settled by a View.** After a write throws, the
  worker recovers and sends the whole database (at most 512 keys) before
  writing again. The checker matches it against the history like a
  recovery, so an unknown write is decided at once and at most one is open
  at a time.
- **Resource limits.** `RLIMIT_AS` and `RLIMIT_NOFILE` are drawn per life,
  either from the start (they land during `DB::open`) or set by the worker
  once the database is open, at its usage then plus a small headroom (they
  land during the run). Under `RLIMIT_AS` the worker runs with
  `MALLOC_MMAP_THRESHOLD_=4096`: glibc otherwise serves allocations from
  arenas reserved at startup, and the limit never bites after open. A
  `std::bad_alloc` from a write is recorded like an I/O error, as an unknown
  outcome the next View settles, so the rig checks the engine's exception
  safety as well. One that escapes a background thread reaches the worker's
  terminate handler, which exits with code 5, the one abort the invariants
  allow. The worker lifts its limits on SIGTERM, since the clean close runs
  with every hazard lifted. The ASan leg disables `RLIMIT_AS`. The cgroup
  memory limit (an OOM kill under reclaim) needs a delegated cgroup on the
  runner and is not implemented yet.
- **`fstat` of an unlinked open file** returns `ENOENT` under chaosfs: the
  kernel sends no handle for it. The engine never calls `fstat`; reads through
  the handle work.
- **After a failed open**, the next life opens with every hazard lifted and
  must succeed (I5); its timeline starts once the database is open.
- **What the next life's power cut may lose.** The checker recovers a copy
  of each life's directory, which includes what only the page cache held.
  That state becomes durable on the mount only when an open there completes.
  So the state file keeps the last state known durable on the mount and the
  writes of the last life that opened on top of it. When a life never opens
  and a power cut or an eviction ends it, the recovered state must be a
  prefix of those writes covering their watermark.
- **Evictions come from chaosfs too.** Within an `fsync_eio` window,
  chaosfs sometimes evicts the pages a failed `fdatasync` lost at once
  (`evict_failed`), so the next read or `resume()` already sees the disk. The
  orchestrator learns whether a life lost pages to eviction from chaosfs's
  own count, not from its timeline.
- **Each life starts from what its open served.** The worker sends the whole
  database with its `Opened` frame. Without a loss before the open, it must
  equal the previous check's recovery; after an eviction that dropped pages,
  a prefix of the pending writes covering their watermark. The life's
  history is then checked from it.
- **An eviction during a life loses data under a running engine.** Pages
  whose background writeback failed are clean but not on disk; evicting them
  drops the published, non-durable writes in them, and the engine cannot
  know until its next `fdatasync` reports the error. After an eviction that
  dropped pages, the checker lets Views lack writes above the durable
  watermark, and does not count a failed read of such a write as
  a violation. A read that returns a value never written still is. If
  `resume()` then refuses to trim published data and asks for a reopen
  (#240), that is the documented recovery: the next life's open must succeed.
- **The clean close calls `DB::close()` (#257).** Returning means every
  acknowledged write is durable, and the checker holds recovery to all of
  them, an eviction before the close included: the engine's last
  `fdatasync` must report a writeback that failed. A `close()` that throws
  has reported the loss; then only the durable watermark binds, and that
  life's writes stay pending for the next power cut.
- **Liveness (I9) is checked at the clean close:** the hazards are lifted,
  and the worker must recover, close and exit within `--close-timeout`
  (60 s), or its stacks are saved and the life fails. Lives that end in a
  kill are not held to a progress bound.
- **No `EINTR`.** A FUSE request fails with `EINTR` only when the caller
  has a signal pending, and the worker handles none but SIGTERM, which ends
  the life. Returning it unprompted would be a behaviour no filesystem has.
- **chaosfs is tested as a model** (`test_chaosfs.py`, run in `ci.yml`), not
  by running `crash_consistency` on it.

## Findings

The first runs, each within minutes, found the bugs listed in
`correctness_validation.md` (*Chaos rig*): #231 in both of its forms (fixed
by #240), #235 (fixed by #247), #236 (fixed by #248) and #237 (fixed by
#255). With #231 fixed, power loss found #245 (fixed by #261). The rig also
motivated `DB::close()` (#257, #260): a clean shutdown whose last
`fdatasync` failed had no way to say so.

All of them are fixed, and `chaos-nightly.yml` runs every hazard.
`KNOWN_BUGS` in the workflow is where a hazard goes when the rig finds a bug
that is not fixed yet, so the nightly can keep finding new ones; each entry
names its issue and goes when the issue is fixed. Against #261, 8 minutes with
every hazard (302 lives, 145 power cuts, 145 evictions, 299 failed
`fdatasync`s, 261 read errors) pass.

## Not covered

- Concurrent group commit under faults is covered by the Elle workload on
  chaosfs (`run_chaos.py --workload elle`, #232), checked per episode, not
  per life.
- Replication under faults and partitions (#178 covers the checker).
- Filesystem behaviour outside the trust boundary above.
