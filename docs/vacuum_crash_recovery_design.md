# Recovering from a kill inside vacuum

Status: proposed. Replaces the approach in PR #131. Fixes #130 and #129.

## Problem

Vacuum compacts a sealed file S into a new file C. The steps are:

1. Write C as `.data.tmp`, fsync, rename to `.data`.
2. Scan C to write its hint file.
3. Commit the new state in memory.
4. Unlink S.

A kill between step 1 and step 4 leaves S and C on disk together. C holds
the live entries of S, copied under their original sequence numbers. On the
next open, recovery sees two entries with the same key and sequence, treats
that as corruption, and refuses to open. A cgroup OOM kill during a sysbench
run hit this window. The database could not be opened afterwards.

The window is not short: step 2 is a full scan of C, which takes seconds for
a 64 MiB file.

## The decision

When recovery finds two files with the same sequence numbers, what should it
do?

- **Refuse to open.** Safe, but a routine kill leaves a database that needs
  manual repair. Not acceptable for an engine whose data files are meant to
  be crash safe.
- **Accept any duplicate sequence.** This is PR #131. `kde_newer` stops
  throwing on equal sequences and picks one entry by file id and offset. It
  still throws if the two value sizes differ.
- **Finish the vacuum.** Recovery deletes S, completing step 4.
- **Undo the vacuum.** Recovery deletes C, returning to the state before
  vacuum started.

We choose to undo. Vacuum is committed on disk when S is unlinked. Before
that, C is unfinished work and recovery discards it, the same way it already
discards a leftover `.data.tmp`. Any duplicate that is not a C and S pair
still refuses to open.

## Why not accept any duplicate

`kde_newer` is shared by every recovery path and checks one entry at a time.
It does not know about vacuum, so a rule placed there applies to every
duplicate, whatever caused it.

Once relaxed, the only corruption it still catches is a pair whose value
sizes differ. Many workloads write fixed-size values (MariaDB rows,
counters, flags). For them, two different writes under one sequence would
be resolved silently. Such a pair can come from a replication bug, a backup
restored into a live directory, or files copied between replicas. Those are
exactly the cases the check exists for.

A stronger per-entry check is not cheap. Hint files carry no per-entry CRC,
so the comparator cannot tell two same-size values apart without reading
data files for every key.

It also leaves S on disk until the next vacuum, and until then
`changes_since` returns S's entries twice (#129).

## Why undo rather than finish

Recovery can prove one thing about the pair: every entry of C is also in S,
with the same sequence, key and CRC.

- **Deleting C** is safe on that proof alone. Everything in C is still in
  S, so nothing is lost, and the result is a directory recovery already
  opened before vacuum ran.
- **Deleting S** also assumes that the entries in S but not in C are dead.
  That is true only if C really came from vacuum and vacuum had no bugs.
  The check does not prove it.

When the assumption fails, finishing loses data. Suppose a data file is
copied while it is still being written, and the copy is later restored
into the directory under another name. The copy is a prefix of the real
file, so it passes the check. Undoing deletes the stale copy and loses
nothing. Finishing deletes the real file and every write after the copy
was taken.

The cost of undoing is that the next vacuum redoes the compaction: one
rewrite of one file, on a background path, after a crash.

The process that was killed had already committed C in memory (step 3), so
after reopening the database is back on S. The data is the same; only the
file set differs. That matters only to something that recorded C's name in
between, such as a `create_manifest` taken in that window, and callers of
`create_manifest` must already keep vacuum from running during a transfer.

## How recovery finds the pair

This runs at open, before the key directory is built.

1. Compute each file's sequence range from its hint file.
2. Find any two files whose ranges overlap. Normally there are none, and
   nothing below runs.
3. For an overlapping pair, call the smaller file C and the larger S. Read
   both data files in order and check that every entry of C, including
   tombstones and batch markers, appears in S with the same sequence, key
   and CRC.
4. If the check passes, delete C and its hint file, then continue recovery.
5. If the check fails, or files overlap in any other way, refuse to open.

The check reads two data files, and only when files overlap, which should
not happen outside this crash. Normal opens pay nothing.

This needs no change to the on-disk format and no new file type. It also
opens directories already in this state, including the one from the OOM
kill.

## What still refuses to open

- Two files with overlapping sequences that are not a C and S pair.
- An entry in C that is missing from S or differs from it.
- Any duplicate sequence the step above did not remove. `kde_newer` keeps
  its current strict check.

## What we keep from PR #131

- `parallel_for` catches exceptions on each thread and rethrows them after
  joining, so a recovery failure is an exception from `DB::open`, not
  `std::terminate`.
- Data files are sorted by name before file ids are assigned, so ids are
  deterministic.
- The `io_vacuum_compact_unlink` fault point and the VC5 proof tests.

We drop the relaxed `kde_newer` and the matching tie-break in
`recovery_build_sorted`.

## Tests

- VC5 (existing, from PR #131): after the fault, reopen and check that C
  is gone from disk, S is still there, and every file's sequence range is
  disjoint. The next vacuum compacts S again.
- Recovery opens a directory holding S and a real compacted C (from vacuum,
  not a byte copy), with serial and parallel recovery agreeing.
- Recovery opens a directory holding a file and a copy of a prefix of it,
  and keeps the full file.
- Recovery refuses a pair that overlaps but is not a C and S pair: an entry
  in C missing from S, or the same sequence with a different value of the
  same size.
- `changes_since` after reopening returns each sequence once (#129).

## Possible later change

If the check at open ever proves too slow, vacuum can write a small intent
file naming S and C before the rename in step 1, and delete it after step 4.
Recovery would then know the pair directly. We are not doing this now
because the check above also covers directories written before such a file
existed.
