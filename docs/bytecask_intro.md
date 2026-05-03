# ByteCaskDB in 5 Minutes

When a database crashes, it wakes up with hard questions. *Did that last write actually happen? Is the disk consistent, or did the crash leave anything broken? Do the index and the data still agree?*

Traditional storage engines answer these questions with a "recovery dance": replaying logs, undoing partial work, and reconciling structures. It works, but it's complicated — and complicated things break in interesting ways.

**ByteCaskDB doesn't have to ask.**

By design, data on disk is always either a complete entry or the absence of one. Entries are never overwritten, so there is no "half-written" state. Because the data files are the sole source of truth, there is no second structure to disagree with. This single decision — **append-only, never mutate** — is the foundation of the entire system.

## The Shape: Where Data Lives, and How You Find It

Think of the data files as the pages of a book, and the key directory as the index in the back. The pages hold the actual content. The index tells you which page to turn to. In ByteCaskDB, the index lives in memory and the durable content lives on disk.

- **Data Files:** These are the durable record. Every operation (Put, Delete, etc.) is appended to an active file as a self-contained entry. When a file reaches a size limit, it is sealed and a new one begins. Nothing is ever modified in place.
- **Key Directory:** An in-memory map that tracks the location of every key (which file, what offset). Two things distinguish it from the original Bitcask design: it's ordered, so range scans and prefix searches are efficient; and it's never mutated in place — readers and writers see different immutable versions, which we'll get to in a moment. It is rebuilt entirely from the data files on startup.

This is a deliberate choice: we trade disk space (stale versions persist until a vacuum cycle) for an **immutable invariant**. Data on disk is never wrong; at worst, it's just waiting to be reclaimed.

## The Engine Cycle: Immutability in Motion

The engine treats state like a functional program would. It never mutates shared structures — not the key directory, not the file handles, not even the statistics. Instead, every change goes through a **Copy-on-Write** cycle:

1. **Snapshot.** The engine starts from the current `EngineState` — an immutable snapshot of the world.
2. **Stage.** It creates a `TransientEngineState`, a private working copy, and applies the change there.
3. **Persist.** The new entries are appended to the active data file and synced to disk.
4. **Publish.** The working copy is frozen into a new `EngineState` and swapped in with a single atomic pointer update.

Readers always hold a reference to whichever `EngineState` was current when they started. They never see a half-finished write, and they never wait for a writer to finish.

## The Payoff

By adopting this immutability philosophy, three classic database headaches simply disappear:

- **Lock-Free Reads:** Readers are never blocked. A reader stays on the version of the `EngineState` it started with, even if a writer publishes five new versions.
- **Zero-Coordination Writers:** Writers don't need to "quiesce" the system or manage complex reader-writer locks. They just swap a pointer when ready.
- **Fast Recovery:** "Recovery" isn't a repair process; it's just bookkeeping. The engine scans the data files to rebuild the Key Directory and resumes. There is no "undo log" because there is nothing to undo.

**The result:** A database that is architecturally simple, inherently concurrent, and remarkably hard to break.