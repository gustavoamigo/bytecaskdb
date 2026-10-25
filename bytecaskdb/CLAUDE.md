# Engine source (`bytecaskdb/`)

> `AGENTS.md` is a symlink to this file. The root `CLAUDE.md` has the rules for the whole repository; this file adds the ones specific to the engine's code, and is read when you work in this folder.

## Iterator laziness — correctness requirement

Iterators must fetch and process data lazily unless explicitly documented otherwise. An iterator that eagerly materializes all results into memory is a correctness bug: the result set may exceed available memory (e.g. a `changes_since` scan over millions of entries). The correct pattern is to hold at most one unit of work in memory at a time (one batch, one entry) and advance on demand.

When implementing an iterator over data files:
- Build the file/partition list eagerly (it is O(num_files), small).
- Scan one batch at a time within each file.
- Cache only the current entry; release previous data on advance.
- If global ordering is required across files, exploit the engine's sequence-disjoint file property (files sorted by min_sequence yield globally ascending order) rather than collecting and sorting all entries.

## Lifetime and view safety — spans, iterators, and non-owning views

The hardest class of bugs in this codebase involves returning non-owning views (`std::span`, `std::string_view`, raw pointers) into an object's own internal buffer. These are correct as long as the owning object stays alive, but silently become dangling references — confirmed by ASan as heap-use-after-free — if:

- The owning object is a temporary (destroyed at end of a full expression).
- The owning object is moved, reallocated, or goes out of scope while the view is still held.
- A standard library adaptor (e.g. `std::reverse_iterator`) creates a copy of your type internally.

**Rule: never compose an iterator whose `operator*` returns a span-into-`*this` with `std::reverse_iterator`.**

`std::reverse_iterator<T>::operator*` is specified as:
```cpp
auto operator*() const { auto tmp = cur_; return *--tmp; }
```
It creates a temporary copy, decrements it, and dereferences it. If `T::operator*` returns a span into `this->buffer_`, that buffer is destroyed when the temporary is destroyed — at the end of the expression — leaving the caller with a dangling span. This also causes a heap alloc+free on every dereference (a performance penalty on top of the UB).

**The correct pattern — hold the cursor alive as a member (lesson from BC-122):**

```cpp
// WRONG: std::reverse_iterator creates temporaries — dangling span, heap alloc per dereference
using reverse_iterator = std::reverse_iterator<RadixTreeIterator<V>>;

// CORRECT: custom reverse iterator holds the cursor alive, operator* returns *cur_
export template <typename V> class ReverseRadixTreeIterator {
public:
  explicit ReverseRadixTreeIterator(RadixTreeIterator<V> past_pos)
      : cur_{std::move(past_pos)} { --cur_; }   // pre-decrement once in ctor

  auto operator*() const { return *cur_; }       // span into cur_'s live buffer — never dangling
  auto operator++() -> ReverseRadixTreeIterator& { --cur_; return *this; }
  auto operator==(std::default_sentinel_t) const noexcept -> bool { return cur_ == std::default_sentinel; }

private:
  RadixTreeIterator<V> cur_;   // owned — keeps the key buffer alive
};
```

**General rules for non-owning views:**

- A function returning `std::span` or `std::string_view` is lending a view. If the owning object's lifetime is not obvious from the call site, document it with a comment: `// returned span valid until next write`.
- Never return a span or view into a local variable, a value parameter, or a temporary.
- Never store a `std::span` or `std::string_view` in a struct unless you can guarantee the owner outlives the struct, and document that guarantee explicitly.
- If you see a `// WARNING:` comment excusing a lifetime hazard, treat it as a required design fix, not permanent documentation. Removing the hazard is always the goal.
- When in doubt, return an owned value (`std::vector<std::byte>`, `std::string`). The borrow optimisation is only worth it when profiling identifies it as a real bottleneck.

## Ownership through failure — prepare, commit, then release

Every owning raw pointer (a key directory node, a buffer, a parcel) has exactly one owner at every instant, including while an exception unwinds. #580 freed a node that the root still reached; #588 found a bulk join that left leaves owned by two loaders. Each became a use-after-free when an allocation failed in that window.

- A mutation of a structure that others can reach, or that its destructor walks, runs in three steps: **prepare** (every allocation, every call that can throw), **commit** (install the result and hand ownership over with operations that cannot throw), **release** (free what was replaced, once nothing reachable points to it). Freeing before the commit point, or allocating after it, is the bug class.
- The structure enforces the order, not its callers: one commit function moves the root and frees what was replaced (`BuildSession::write`), and a commit step that must not throw is `noexcept` (`BulkLoader::adopt_runs`), so a throw there ends the process rather than leaving a pointer with two owners.
- A mutation that fails partway and cannot be rolled back leaves the object refusing every use except destruction (`BuildSession::usable`). Never let a caller keep writing to half-changed state.
- Moving ownership of a batch of pointers is one non-throwing step, or one pointer at a time with the source cleared as each moves. Never copy pointers to the new owner and clear the old owner afterwards, with a throwing call in between.

## Batch marker invariants

BulkBegin/BulkEnd markers must NOT be skipped by vacuum or ChangeIterator — they are structural markers that preserve batch atomicity on disk. Only hint generation skips them (hints do not have the concept).

## Directory listing on a failure path

Prefer `std::filesystem` utilities over manual file-handle tricks (e.g. `std::filesystem::file_size`). The exception is listing a directory on a path that must survive a failed allocation: libstdc++'s `directory_iterator` allocates inside a `noexcept` function, so the failure ends the process (#598). Recovery uses `list_directory` (`data_file.cppm`) instead; `alloc sweep: open` and the `recovery_lists_with_directory_iterator` mutation hold it there.

## Tests

A change to recovery, the key directory trees, a durability site or the write path usually needs test changes: read `tests/CLAUDE.md` (fault sweeps, model-based recovery tests, mutation sets) before finishing it.
