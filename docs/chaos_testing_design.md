# Chaos Testing — Design

> Status: first version implemented (`tests/chaos/`, `chaos-nightly.yml`).
> Tracks issue #230; the cgroup memory hazard and the Elle workload on
> chaosfs (#232) are still to come.

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
does. A mutation set in `tests/chaos_mutations/`, run by a script like
`soak_mutation_check.sh`, must each be caught within a fixed budget:

| Mutation | Expected catch |
|---|---|
| No `fdatasync` before sealing at rotation | power loss loses a durable write (I1) |
| No directory sync after creating a data file (#199) | power loss drops the file's name (I1) |
| No sync before degrading (B1–B3) | power loss after a failed write and `resume()` (I6) |
| Hint written in place, not temp-then-rename | expected to survive: the torn hint fails its CRC and is rebuilt — recorded to confirm the rig reaches it |
| `resume()` skips the scan and reuses the active file | I2 or I6 |

A mutation expected to be caught that survives is a rig bug.

The first question the rig answered is not a mutation. After an `fdatasync`
fails, `resume()` "replays any valid committed entries (including F/G
bytes if they survived in the page cache to `sync()`)". Under the Linux
semantics above those bytes are readable but no longer headed for the disk,
and the `sync()` after them succeeds without writing them. The rig
reproduced it (#231): after the power loss, the hint `resume()` wrote indexes
zeroed records and the database no longer opens. The mutation set is still to
be built.

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
`correctness_validation.md` (*Chaos rig*): #231 in both of its forms, #235,
#236 and #237. Until they are fixed, a full run fails within tens of lives.
`--disable power,fsync_eio,writeback_fail,evict` runs past #231, `meta_eio` past
#236 and `read_eio` past #237. With all of those left out, runs of 15 minutes
(over 400 lives and 950,000 checked writes) pass.

## Not covered

- Concurrent group commit under faults. A later step runs the Elle
  workload (`tests/elle/isolation_history.cpp`) on chaosfs, which adds
  multi-writer histories without a new oracle.
- Replication under faults and partitions (#178 covers the checker).
- Filesystem behaviour outside the trust boundary above.
