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
- **Finish the vacuum that was cut off.** Recovery proves that C is a
  compaction of S, deletes S, then opens as usual. Every other duplicate
  still refuses to open.

We choose the third option.

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

There are two smaller costs:

- Until the next vacuum removes S, `changes_since` returns S's entries
  twice (#129).
- The tie-break assumes C sorts after S by name. Names sort by creation
  time only to the second. Within a second the random salt decides, so
  about half the time S would keep the live bytes and the next vacuum would
  rewrite it instead of deleting it.

## Why finishing the vacuum is safe

Vacuum only renames C into place after C is fsynced. So if C exists as
`.data`, it is complete, and step 4 (unlinking S) is the only step left.
Recovery completing it produces the same directory a vacuum that was not
interrupted would have left.

## How recovery proves C is a compaction of S

This runs at open, before the key directory is built.

1. Compute each file's sequence range from its hint file.
2. Find any two files whose ranges overlap. Normally there are none, and
   nothing below runs.
3. For an overlapping pair, call the smaller file C and the larger S. Read
   both data files in order and check that every entry of C, including
   tombstones and batch markers, appears in S with the same sequence, key
   and CRC.
4. If the check passes, delete S and its hint file, then continue recovery.
5. If the check fails, or files overlap in any other way, refuse to open.

The check reads two data files, and only when files overlap, which should
not happen outside this crash. Normal opens pay nothing.

This needs no change to the on-disk format and no new file type. It also
opens directories already in this state, including the one from the OOM
kill.

## What still refuses to open

- Two files with overlapping sequences that are not a compaction pair.
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

- VC5 (existing, from PR #131): after the fault, reopen and also check that
  S is gone from disk and every file's sequence range is disjoint.
- Recovery opens a directory holding S and a real compacted C (from vacuum,
  not a byte copy), with serial and parallel recovery agreeing.
- Recovery refuses a pair that overlaps but is not a compaction: an entry
  in C missing from S, or the same sequence with a different value of the
  same size.
- `changes_since` after reopening returns each sequence once (#129).

## Possible later change

If the check at open ever proves too slow or too loose, vacuum can write a
small intent file naming S before the rename in step 1. Recovery would then
know the pair directly instead of deriving it. We are not doing this now
because the check above also covers directories written before such a file
existed.
