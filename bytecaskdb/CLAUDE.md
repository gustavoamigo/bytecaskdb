# Engine source (`bytecaskdb/`)

> `AGENTS.md` is a symlink to this file. The root `CLAUDE.md` has the rules for the whole repository; this file adds the ones for engine code, and is read when you work in this folder.

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
