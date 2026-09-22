// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — version-chain reclamation for persistent trees.
//
// Lifted from the radix tree (PR #86) with the tree shape abstracted behind
// a Traits type, so any path-copying tree can use it. See
// docs/radix_tree_epoch_reclamation_design.md for the reasoning and
// docs/persistent_btree_design.md for the B+ tree that uses it.

module;
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

export module bytecask.version_chain;

namespace bytecask {

// Session tags. Relaxed ordering: only uniqueness is required. A tag names
// the session that created a node and the version that session published;
// it decides whether a node may be mutated in place, so it must be unique
// for the lifetime of the process.
export inline std::atomic<std::uint64_t> next_version_tag{1};

export inline auto new_version_tag() noexcept -> std::uint64_t {
  return next_version_tag.fetch_add(1, std::memory_order_relaxed);
}

// What a tree supplies to the chain:
//
//   using Node = ...;
//   static auto tag(const Node *) -> std::uint64_t;       // creating session
//   template <typename F> static void for_each_child(Node *, F &&); // F(Node*)
//   static void destroy(Node *) noexcept;                  // free one node
//   static void account_retired(std::int64_t) noexcept;    // test accounting
//
// Walks the subtree under `root`, descending through every node for which
// `is_garbage(node)` holds and freeing it. The predicate must be monotone
// along a path — once it is false for a node it is false for everything
// below it — which holds for both predicates used here, because a node's
// children are never newer than the node itself: a session links new
// children only into nodes it owns. Iterative — no recursion on tree depth.
export template <typename Traits, typename Pred>
void free_node_subtree_if(typename Traits::Node *root, Pred is_garbage) {
  using Node = typename Traits::Node;
  if (!root || !is_garbage(root))
    return;
  std::vector<Node *> stack;
  stack.push_back(root);
  while (!stack.empty()) {
    auto *n = stack.back();
    stack.pop_back();
    Traits::for_each_child(n, [&](Node *child) {
      if (is_garbage(child))
        stack.push_back(child);
    });
    Traits::destroy(n);
  }
}

// ---------------------------------------------------------------------------
// VersionChain<Traits> — owns node lifetime for every persistent tree built
// over one Traits type.
//
// Both trees instantiate this: the B+ tree over btree_detail::ChainTraits and
// the radix tree over RadixChainTraits. It began as the radix tree's own
// class and was lifted out and made generic over Traits, which needs only a
// node's version tag, its children, how to destroy it, and the accounting
// hook the memory tests read.
//
// A persistent tree is a *version*, identified by the tag of the session
// that built it, and registered here while any handle to it lives. The
// session retires the base nodes it makes unreachable — the ones it clones
// or unlinks — and hands that list over when it publishes. Nothing carries a
// reference count; freeing is a batch of plain deletes off the write path.
//
// Two contracts make the free rule exact. publish() enforces both.
//
//   Chain. A version may be derived only from a version that has no
//   successor. The versions derived from one another form a lineage, and
//   within it the tags order the versions. Tags increase with every
//   session, and a session starts only after its base was published, so a
//   node created by session S is reachable only from S's version and the
//   ones derived from it, and a node retired by session R is reachable from
//   none of R's version or its descendants. A node created by S and retired
//   by R is therefore reachable from exactly the versions of its lineage
//   with tags in [S, R).
//
//   Consumption. merge takes the sole handle of two versions that have
//   neither predecessor nor successor — what a builder or a previous merge
//   yields — and its result starts a new lineage. The inputs stop being
//   versions at publish, and the nodes of theirs the result does not reuse
//   are freed at once: nothing else reached them.
//
// When is a retired node freed? It is parked on the live version that
// blocks it — the smallest live tag of its lineage in [S, R) — and freed the
// moment no such version exists: at publish when none is alive, otherwise
// when the version it is parked on dies and nothing in the window is left.
// That test is blocker_for. A version's death re-examines only the nodes
// parked on it, never a scan, so a long-lived snapshot holds exactly the
// nodes it can still reach, not everything retired since it was taken.
//
// Retraction. When a version with no successor loses its last handle, the
// lineage shrinks back to its newest live predecessor F, or ends if there is
// none. Everything above F is a dead segment nothing reaches any more:
//   - nodes reachable from the dead root with a tag above F were created by
//     the segment: freed by a walk, which stops where the test fails since
//     children are never newer than their parent;
//   - nodes retired by the segment — retiring tag above F — are reachable
//     from F again and are unparked: live nodes of F once more. With no F
//     the lineage is over and they are freed as well.
// F is then the head of its lineage again. This one rule covers the head
// dropped after a failed flush (DB::resume), a version a test publishes and
// drops, the last handle at DB close, and the tail of a recovery partition.
//
// Reclamation runs on the thread that drops the last handle. All state is
// under one mutex, and the freeing of retired nodes happens after it is
// released; the retraction walk is the exception and holds it, because once
// a version leaves the chain another thread may decide that what it reached
// is free while the walk is still stepping through those nodes.
//
// There are only ever a handful of versions, so they live in a flat vector
// sorted by tag and searched linearly, and retired nodes stay in the vector
// the session already built. Publishing a version then allocates nothing
// once those buffers have settled, which matters because the engine
// publishes one per batch. The buffers are handed back when the last
// version of this value type goes.
// ---------------------------------------------------------------------------
export template <typename Traits> class VersionChain {
public:
  using Node = typename Traits::Node;

  static auto instance() -> VersionChain & {
    // Immortal: a tree can outlive static destruction, so the chain is
    // constructed once in static storage and never destroyed. Static
    // storage rather than the heap, so a leak checker sees nothing left
    // behind at exit.
    alignas(VersionChain) static std::byte storage[sizeof(VersionChain)];
    static auto *chain = new (storage) VersionChain();
    return *chain;
  }

  // Registers the version built by session `tag` from `base` (0 = the empty
  // tree, which starts a lineage). Throws std::logic_error if `base` already
  // has a successor — the chain contract — with nothing changed and
  // `retired` still the session's. On success `retired` is taken over and
  // left empty. The base is live: the builder holds a handle to it.
  void publish(std::uint64_t tag, std::uint64_t base,
               std::vector<Node *> &retired) {
    std::vector<Node *> to_free;
    {
      std::lock_guard<std::mutex> lk{mu_};
      auto lineage = tag;
      if (base != 0) {
        auto *b = find(base);
        if (b->successor != 0)
          throw std::logic_error{
              "VersionChain: a version that already has a successor cannot "
              "be derived from again"};
        b->successor = tag;
        lineage = b->lineage;
      }
      add_version(tag, lineage);
      park_retired(tag, lineage, retired, to_free);
    }
    destroy_all(to_free);
  }

  // Registers the version built by session `tag` merging versions `a` and
  // `b` (0 = the empty tree), consuming both: each must be held by exactly
  // one handle and be the only version of its lineage, or std::logic_error
  // is thrown with nothing changed. On success the inputs are no longer
  // versions — the caller drops its handles without unpinning — and the
  // result starts a lineage of its own.
  void publish_merge(std::uint64_t tag, std::uint64_t a, std::uint64_t b,
                     std::vector<Node *> &retired) {
    std::vector<Node *> to_free;
    {
      std::lock_guard<std::mutex> lk{mu_};
      for (auto input : {a, b}) {
        if (input != 0)
          check_consumable(input);
      }
      for (auto input : {a, b}) {
        if (input != 0)
          records_.erase(seat_for(input));
      }
      add_version(tag, tag);
      park_retired(tag, tag, retired, to_free);
    }
    destroy_all(to_free);
  }

  void pin(std::uint64_t id) {
    std::lock_guard<std::mutex> lk{mu_};
    ++find(id)->live;
  }

  // Drops one handle of `id`. `root` is walked only when this was the last
  // handle and the version has no successor — see the class comment.
  void unpin(std::uint64_t id, Node *root) {
    std::vector<Node *> to_free;
    {
      std::lock_guard<std::mutex> lk{mu_};
      auto it = seat_for(id);
      assert(it != records_.end() && it->tag == id);
      if (--it->live > 0)
        return;
      if (it->successor != 0) {
        // Dead inside the chain: its successor reaches everything it did,
        // except what was parked here — that moves on to the next version
        // still reaching it, or is freed.
        take_parcels_if(*it, [](const Parcel &) { return true; });
        records_.erase(it);
      } else {
        retract(it, root, to_free);
      }
      drain_pending(to_free);
      release_buffers_if_idle();
    }
    destroy_all(to_free);
  }

  // What an operator can see of reclamation: how many versions are alive —
  // one is the published state, every further one is a snapshot, an
  // iterator or a thread's read cache holding an older tree — and how many
  // retired nodes those older versions are keeping from being freed. Under
  // random writes a version held long enough ends up holding the whole
  // tree it was taken from, so parked nodes climbing towards the node count
  // while versions stay above one names a stale holder, not a leak.
  struct Gauges {
    std::size_t versions{0};
    std::size_t parked_nodes{0};
  };
  [[nodiscard]] auto gauges() -> Gauges {
    std::lock_guard<std::mutex> lk{mu_};
    Gauges g{records_.size(), 0};
    for (const auto &rec : records_) {
      g.parked_nodes += rec.parked.nodes.size();
      for (const auto &parcel : rec.more)
        g.parked_nodes += parcel.nodes.size();
    }
    for (const auto &parcel : pending_)
      g.parked_nodes += parcel.nodes.size();
    return g;
  }

  // Test-only: every retired node still waiting for a live version to
  // release it.
  [[nodiscard]] auto parked_nodes() -> std::vector<const void *> {
    std::lock_guard<std::mutex> lk{mu_};
    std::vector<const void *> out;
    auto add = [&out](const Parcel &parcel) {
      out.insert(out.end(), parcel.nodes.begin(), parcel.nodes.end());
    };
    for (const auto &rec : records_) {
      add(rec.parked);
      for (const auto &parcel : rec.more)
        add(parcel);
    }
    for (const auto &parcel : pending_)
      add(parcel);
    return out;
  }

private:
  // Nodes retired by one session. `retired_by` closes their reachability
  // window: a version of `lineage` with a tag at or above it was derived
  // from that session and cannot reach them.
  struct Parcel {
    std::vector<Node *> nodes;
    std::uint64_t retired_by{0};
    std::uint64_t lineage{0};
  };

  // One live version. Kept in one vector sorted by tag: there are a handful
  // of them, so a flat vector searches faster than a node-based container
  // and, once its capacity has settled, publishing allocates nothing.
  struct Record {
    std::uint64_t tag{0};
    std::uint64_t lineage{0};   // tag of the first version of its lineage
    std::uint64_t successor{0}; // tag of the version derived from it, or 0
    std::uint32_t live{0};      // handles
    // Nodes parked on this version, because it is the live version that
    // still reaches them. One parcel inline covers the usual case — one
    // published version's worth of superseded nodes — so parking allocates
    // nothing; further ones spill into `more`.
    Parcel parked;
    std::vector<Parcel> more;
  };

  std::mutex mu_;
  std::vector<Record> records_;
  // Parcels taken off a record and waiting to be placed again or released.
  // Scratch reused under mu_, so the steady state allocates nothing.
  std::vector<Parcel> pending_;

  // Where `tag` belongs in the sorted record vector.
  [[nodiscard]] auto seat_for(std::uint64_t tag)
      -> typename std::vector<Record>::iterator {
    return std::lower_bound(records_.begin(), records_.end(), tag,
                            [](const Record &r, std::uint64_t t) {
                              return r.tag < t;
                            });
  }
  [[nodiscard]] auto find(std::uint64_t tag) -> Record * {
    auto it = seat_for(tag);
    assert(it != records_.end() && it->tag == tag);
    return &*it;
  }

  // Under mu_: a new live version with one handle.
  void add_version(std::uint64_t tag, std::uint64_t lineage) {
    Record rec;
    rec.tag = tag;
    rec.lineage = lineage;
    rec.live = 1;
    records_.insert(seat_for(tag), std::move(rec));
  }

  // Under mu_: takes the session's retired list over as one parcel and
  // places it. Leaves `retired` empty.
  void park_retired(std::uint64_t tag, std::uint64_t lineage,
                    std::vector<Node *> &retired, std::vector<Node *> &out) {
    if (!retired.empty())
      pending_.push_back(Parcel{std::move(retired), tag, lineage});
    retired.clear();
    drain_pending(out);
  }

  // Under mu_: the consumption contract for one merge input.
  void check_consumable(std::uint64_t tag) {
    const auto *rec = find(tag);
    if (rec->live != 1)
      throw std::logic_error{
          "VersionChain::merge: an input is held by another handle"};
    if (rec->successor != 0)
      throw std::logic_error{"VersionChain::merge: an input has a successor"};
    for (const auto &r : records_) {
      if (r.lineage == rec->lineage && r.tag != tag)
        throw std::logic_error{
            "VersionChain::merge: an input has a live predecessor"};
    }
    // Nothing can be parked on the only version of a lineage.
    assert(rec->parked.nodes.empty() && rec->more.empty());
  }

  // Under mu_: the version at `it` has no successor and no handle left.
  // Frees what the dead segment above the newest live predecessor created,
  // unparks what it retired, and makes that predecessor the head again —
  // see the class comment.
  void retract(typename std::vector<Record>::iterator it, Node *root,
               std::vector<Node *> &out) {
    const auto tag = it->tag;
    const auto lineage = it->lineage;
    Record *pred = nullptr;
    for (auto r = it; r != records_.begin();) {
      --r;
      if (r->lineage == lineage) {
        pred = &*r;
        break;
      }
    }
    const auto floor = pred ? pred->tag : 0;
    // Under the lock: once this version leaves the chain another thread may
    // decide that what it reached is free while this walk is still stepping
    // through those nodes to reach the ones below them.
    free_node_subtree_if<Traits>(root, [floor](Node *n) {
      return Traits::tag(n) > floor;
    });
    // What the segment retired is parked on versions of this lineage at or
    // below it.
    for (auto &r : records_) {
      if (r.lineage == lineage && r.tag <= tag)
        take_parcels_if(r, [floor](const Parcel &p) {
          return p.retired_by > floor;
        });
    }
    for (auto &parcel : pending_) {
      if (pred)
        Traits::account_retired(
            -static_cast<std::int64_t>(parcel.nodes.size()));
      else
        out.insert(out.end(), parcel.nodes.begin(), parcel.nodes.end());
    }
    pending_.clear();
    if (pred)
      pred->successor = 0;
    records_.erase(it);
  }

  // Under mu_: moves every parcel of `rec` for which `pred` holds onto the
  // pending queue.
  template <typename Pred> void take_parcels_if(Record &rec, Pred pred) {
    if (!rec.parked.nodes.empty() && pred(rec.parked)) {
      pending_.push_back(std::move(rec.parked));
      rec.parked.nodes.clear();
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < rec.more.size(); ++i) {
      if (pred(rec.more[i])) {
        pending_.push_back(std::move(rec.more[i]));
        continue;
      }
      if (kept != i)
        rec.more[kept] = std::move(rec.more[i]);
      ++kept;
    }
    rec.more.resize(kept);
  }

  // Under mu_: places every pending parcel. Runs after the record vector
  // has stopped moving, so hold() can take a pointer into it.
  void drain_pending(std::vector<Node *> &out) {
    while (!pending_.empty()) {
      auto parcel = std::move(pending_.back());
      pending_.pop_back();
      park(std::move(parcel), out);
    }
  }

  // The free rule. The live version that still reaches a node created by
  // session `node_tag` and retired by `parcel.retired_by`: the smallest live
  // tag of the parcel's lineage in [node_tag, retired_by). Returns 0 when
  // no version reaches the node any more and it can be freed.
  [[nodiscard]] auto blocker_for(std::uint64_t node_tag, const Parcel &parcel)
      -> std::uint64_t {
    for (auto it = seat_for(node_tag);
         it != records_.end() && it->tag < parcel.retired_by; ++it) {
      if (it->lineage == parcel.lineage)
        return it->tag;
    }
    return 0;
  }

  // Under mu_: sends each node of `parcel` to the live version that still
  // reaches it, or to `out` when none does. The nodes of one parcel almost
  // always share a blocker — they came off one path in one version — so
  // that case moves the whole list and allocates nothing.
  void park(Parcel parcel, std::vector<Node *> &out) {
    const auto first = blocker_for(Traits::tag(parcel.nodes.front()), parcel);
    bool uniform = true;
    for (auto *n : parcel.nodes) {
      if (blocker_for(Traits::tag(n), parcel) != first) {
        uniform = false;
        break;
      }
    }
    if (uniform) {
      if (first == 0)
        out.insert(out.end(), parcel.nodes.begin(), parcel.nodes.end());
      else
        hold(first, std::move(parcel));
      return;
    }
    // Mixed: split by blocker. Rare, and the split lists are short.
    std::vector<std::pair<std::uint64_t, Parcel>> groups;
    for (auto *n : parcel.nodes) {
      const auto blocker = blocker_for(Traits::tag(n), parcel);
      if (blocker == 0) {
        out.push_back(n);
        continue;
      }
      auto group = std::find_if(groups.begin(), groups.end(),
                                [blocker](const auto &g) {
                                  return g.first == blocker;
                                });
      if (group == groups.end())
        groups.emplace_back(blocker, Parcel{{n}, parcel.retired_by,
                                            parcel.lineage});
      else
        group->second.nodes.push_back(n);
    }
    for (auto &[blocker, group] : groups)
      hold(blocker, std::move(group));
  }

  // Under mu_: hands a parcel to the version that still reaches it.
  void hold(std::uint64_t blocker, Parcel parcel) {
    auto *rec = find(blocker);
    if (rec->parked.nodes.empty())
      rec->parked = std::move(parcel);
    else
      rec->more.push_back(std::move(parcel));
  }

  // Under mu_: when the last version of every tree of this value type is
  // gone, hand the chain's own buffers back too. They are reused and never
  // shrink otherwise, which is what makes publishing allocation-free in the
  // steady state — but an idle process should not be holding them, and the
  // tree is then provably responsible for no memory at all, which the
  // memory tests check.
  void release_buffers_if_idle() {
    if (!records_.empty() || !pending_.empty())
      return;
    records_.shrink_to_fit();
    pending_.shrink_to_fit();
  }

  static void destroy_all(const std::vector<Node *> &nodes) noexcept {
    if (nodes.empty())
      return;
    Traits::account_retired(-static_cast<std::int64_t>(nodes.size()));
    for (auto *n : nodes)
      Traits::destroy(n);
  }
};

} // namespace bytecask
