#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Simulates UnorderedView write amplification under different parameter
# configurations. Models linear hashing splits, hash16 collision rates,
# bloom filter false positives, and chain rewriting to produce accurate
# byte-level write amplification estimates.
#
# Usage:
#   python3 scripts/simulate_write_amp.py
#   python3 scripts/simulate_write_amp.py --keys 500000 --key-size 36 --value-size 100

import argparse
import math
import random
from dataclasses import dataclass


# ---------------------------------------------------------------------------
# ByteCaskDB on-disk constants (from docs/file_format.md)
# ---------------------------------------------------------------------------
HEADER_SIZE = 15       # sequence(8) + entry_type(1) + key_size(2) + value_size(4)
CRC_SIZE = 4           # trailing CRC-32C
ENTRY_OVERHEAD = HEADER_SIZE + CRC_SIZE  # 19 bytes per data file entry

# UnorderedView chain encoding overhead per entry in a chain value
CHAIN_ENTRY_OVERHEAD = 8  # key_len(4) + val_len(4)


# ---------------------------------------------------------------------------
# Simulation state
# ---------------------------------------------------------------------------
@dataclass
class SimStats:
    logical_puts: int = 0
    path1_append: int = 0           # slot absent — pure append
    path2_bloom_skip: int = 0       # bloom no-collision — direct overwrite
    path2_bloom_collision: int = 0  # bloom detected first collision on slot
    path3_rmw_update: int = 0       # bloom maybe-collision, key exists
    path3_rmw_new: int = 0          # bloom maybe-collision, new key in slot
    splits: int = 0
    split_entries_moved: int = 0
    split_slots_deleted: int = 0
    split_slots_written: int = 0
    hash16_true_collisions: int = 0
    bytes_logical: int = 0
    bytes_written: int = 0


@dataclass
class Options:
    initial_size: int = 8
    bucket_capacity: int = 64
    load_factor: float = 0.75
    bloom_fp_rate: float = 0.01


class Simulator:
    def __init__(self, opts: Options, key_size: int, value_size: int,
                 ns: str = "uv", seed: int = 42):
        self.opts = opts
        self.key_size = key_size
        self.value_size = value_size
        self.slot_key_size = len(ns) + 3 + 4 + 2  # ns + "/b/" + bucket(4) + fp(2)
        self.meta_key_size = len(ns) + 9           # "/__meta__"
        self.bloom_key_size = len(ns) + 10         # "/__bloom__"
        self.rng = random.Random(seed)

        # Linear hashing state
        self.initial_size = opts.initial_size
        self.split_pointer = 0
        self.round = 0
        self.entry_count = 0

        # Slots: (bucket_id, fp16) -> set of key_id
        self.slots: dict[tuple[int, int], set[int]] = {}

        # Tracks which slots have a known collision (bloom bit set)
        self.known_collisions: set[tuple[int, int]] = set()

        # Per-key: precomputed (hash32, hash16). Filled lazily.
        self._h32: dict[int, int] = {}
        self._h16: dict[int, int] = {}

        # Bloom filter size (for byte accounting only)
        expected = max(1, self.initial_size * self.opts.bucket_capacity)
        m = math.ceil(-(expected * math.log(self.opts.bloom_fp_rate)) /
                      (math.log(2) ** 2))
        m = (m + 7) & ~7
        self.bloom_bytes = 8 + m // 8

        self.stats = SimStats()

    def _key_hashes(self, key_id: int) -> tuple[int, int]:
        """Derive deterministic hash32/hash16 for a key. Uses low bits
        as h16 and upper bits as h32 — uniform enough for simulation."""
        if key_id not in self._h32:
            # Splitmix64 — fast, uniform, deterministic
            x = (key_id + 1) * 0x9E3779B97F4A7C15 & 0xFFFFFFFFFFFFFFFF
            x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
            x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
            x = x ^ (x >> 31)
            self._h32[key_id] = (x >> 32) & 0xFFFFFFFF
            self._h16[key_id] = x & 0xFFFF
        return self._h32[key_id], self._h16[key_id]

    def _route(self, h32: int) -> int:
        n = self.initial_size * (1 << self.round)
        bucket = h32 % n
        if bucket < self.split_pointer:
            bucket = h32 % (2 * n)
        return bucket

    def _num_buckets(self) -> int:
        return self.initial_size * (1 << self.round) + self.split_pointer

    def _chain_bytes(self, count: int) -> int:
        return count * (CHAIN_ENTRY_OVERHEAD + self.key_size + self.value_size)

    def _db_put_bytes(self, key_sz: int, val_sz: int) -> int:
        return ENTRY_OVERHEAD + key_sz + val_sz

    def _db_del_bytes(self, key_sz: int) -> int:
        return ENTRY_OVERHEAD + key_sz  # tombstone: value_size=0

    def _bloom_says_collision(self, slot_id: tuple[int, int]) -> bool:
        """Simulates bloom filter: definitive if in known_collisions,
        random false positive otherwise."""
        if slot_id in self.known_collisions:
            return True
        return self.rng.random() < self.opts.bloom_fp_rate

    def put(self, key_id: int):
        self.stats.logical_puts += 1
        self.stats.bytes_logical += ENTRY_OVERHEAD + self.key_size + self.value_size

        h32, fp = self._key_hashes(key_id)
        bucket = self._route(h32)
        sid = (bucket, fp)

        slot = self.slots.get(sid)

        if slot is None:
            # PATH 1: slot absent — pure append, 1 write
            self.stats.path1_append += 1
            self.slots[sid] = {key_id}
            self.stats.bytes_written += self._db_put_bytes(
                self.slot_key_size, self._chain_bytes(1))
            self.entry_count += 1

        elif not self._bloom_says_collision(sid):
            # PATH 2: bloom says no collision — check if same key
            if len(slot) == 1 and key_id in slot:
                # Same key update — direct overwrite
                self.stats.path2_bloom_skip += 1
                self.stats.bytes_written += self._db_put_bytes(
                    self.slot_key_size, self._chain_bytes(1))
            else:
                # First collision on this slot
                self.stats.path2_bloom_collision += 1
                self.stats.hash16_true_collisions += 1
                self.known_collisions.add(sid)
                if key_id not in slot:
                    slot.add(key_id)
                    self.entry_count += 1
                # RMW: rewrite chain + bloom update (atomic batch = 2 puts)
                self.stats.bytes_written += self._db_put_bytes(
                    self.slot_key_size, self._chain_bytes(len(slot)))
                self.stats.bytes_written += self._db_put_bytes(
                    self.bloom_key_size, self.bloom_bytes)
        else:
            # PATH 3: bloom says maybe collision — full RMW
            if key_id in slot:
                self.stats.path3_rmw_update += 1
            else:
                self.stats.path3_rmw_new += 1
                slot.add(key_id)
                self.entry_count += 1
                if len(slot) > 1:
                    self.known_collisions.add(sid)
                    self.stats.hash16_true_collisions += 1
                    # Bloom update
                    self.stats.bytes_written += self._db_put_bytes(
                        self.bloom_key_size, self.bloom_bytes)
            self.stats.bytes_written += self._db_put_bytes(
                self.slot_key_size, self._chain_bytes(len(slot)))

        # Split check
        threshold = int(self._num_buckets() * self.opts.bucket_capacity *
                        self.opts.load_factor)
        if self.entry_count > threshold:
            self._split()

    def _split(self):
        self.stats.splits += 1
        n = self.initial_size * (1 << self.round)
        old_bucket = self.split_pointer
        new_mod = 2 * n

        # Collect slots in old bucket
        old_sids = [s for s in self.slots if s[0] == old_bucket]

        if not old_sids:
            self.stats.bytes_written += self._db_put_bytes(self.meta_key_size, 16)
            self._advance(n)
            return

        # Re-route all entries
        new_chains: dict[tuple[int, int], set[int]] = {}
        for sid in old_sids:
            for kid in self.slots[sid]:
                h32, fp = self._key_hashes(kid)
                new_bucket = h32 % new_mod
                new_sid = (new_bucket, fp)
                if new_sid not in new_chains:
                    new_chains[new_sid] = set()
                new_chains[new_sid].add(kid)
                self.stats.split_entries_moved += 1

        # Delete old slots
        for sid in old_sids:
            del self.slots[sid]
            self.known_collisions.discard(sid)
            self.stats.bytes_written += self._db_del_bytes(self.slot_key_size)
            self.stats.split_slots_deleted += 1

        # Write new slots
        for new_sid, keys in new_chains.items():
            self.slots[new_sid] = keys
            if len(keys) > 1:
                self.known_collisions.add(new_sid)
            self.stats.bytes_written += self._db_put_bytes(
                self.slot_key_size, self._chain_bytes(len(keys)))
            self.stats.split_slots_written += 1

        # Metadata
        self.stats.bytes_written += self._db_put_bytes(self.meta_key_size, 16)
        self._advance(n)

    def _advance(self, n: int):
        self.split_pointer += 1
        if self.split_pointer >= n:
            self.split_pointer = 0
            self.round += 1

    def write_amp(self) -> float:
        return self.stats.bytes_written / self.stats.bytes_logical if self.stats.bytes_logical else 0.0


def run(num_keys: int, key_size: int, value_size: int,
        opts: Options, updates_pct: float = 0.0,
        label: str = "", verbose: bool = True) -> tuple[float, SimStats]:
    sim = Simulator(opts, key_size, value_size)

    for i in range(num_keys):
        sim.put(i)

    num_updates = int(num_keys * updates_pct)
    for i in range(num_updates):
        sim.put(i % num_keys)

    s = sim.stats
    wa = sim.write_amp()

    if verbose:
        if label:
            print(f"\n{'=' * 72}")
            print(f"  {label}")
            print(f"{'=' * 72}")
        print(f"  Keys: {num_keys:,}  Key: {key_size}B  Value: {value_size}B"
              f"  Updates: {num_updates:,}")
        print(f"  initial_size={opts.initial_size}"
              f"  bucket_capacity={opts.bucket_capacity}"
              f"  load_factor={opts.load_factor}"
              f"  bloom_fp={opts.bloom_fp_rate}")
        print(f"  Buckets: {sim._num_buckets():,}  Rounds: {sim.round}"
              f"  Splits: {s.splits:,}  Entries moved: {s.split_entries_moved:,}")
        print(f"  Path1(append)={s.path1_append:,}"
              f"  Path2(skip)={s.path2_bloom_skip:,}"
              f"  Path2(coll)={s.path2_bloom_collision:,}"
              f"  Path3(upd)={s.path3_rmw_update:,}"
              f"  Path3(new)={s.path3_rmw_new:,}")
        print(f"  Hash16 collisions: {s.hash16_true_collisions:,}")
        print(f"  Bytes logical: {s.bytes_logical:,}"
              f"  written: {s.bytes_written:,}")
        print(f"  >>> Write amplification: {wa:.2f}x")

    return wa, s


def table(rows: list[tuple[str, float]]):
    print(f"\n  {'Config':<32} {'WA':>6}")
    print(f"  {'-'*32} {'-'*6}")
    for label, wa in rows:
        print(f"  {label:<32} {wa:>5.2f}x")


def main():
    p = argparse.ArgumentParser(description="Simulate UnorderedView write amp")
    p.add_argument("--keys", type=int, default=1_000_000)
    p.add_argument("--key-size", type=int, default=36)
    p.add_argument("--value-size", type=int, default=100)
    p.add_argument("--updates-pct", type=float, default=0.0)
    args = p.parse_args()

    N, KS, VS, UP = args.keys, args.key_size, args.value_size, args.updates_pct
    print(f"UnorderedView Write Amplification Simulator")
    print(f"============================================")
    print(f"Base: {N:,} keys, {KS}B keys, {VS}B values\n")

    # bucket_capacity sweep
    print("### bucket_capacity sweep")
    rows = []
    for cap in [16, 32, 64, 128, 256]:
        wa, _ = run(N, KS, VS, Options(bucket_capacity=cap), UP,
                    f"bucket_capacity={cap}")
        rows.append((f"bucket_capacity={cap}", wa))
    table(rows)

    # load_factor sweep
    print("\n### load_factor sweep")
    rows = []
    for lf in [0.50, 0.60, 0.75, 0.85, 0.95]:
        wa, _ = run(N, KS, VS, Options(load_factor=lf), UP,
                    f"load_factor={lf}")
        rows.append((f"load_factor={lf}", wa))
    table(rows)

    # initial_size sweep
    print("\n### initial_size sweep")
    rows = []
    for isz in [4, 8, 16, 64, 256]:
        wa, _ = run(N, KS, VS, Options(initial_size=isz), UP,
                    f"initial_size={isz}")
        rows.append((f"initial_size={isz}", wa))
    table(rows)

    # key/value size sensitivity
    print("\n### key/value size sensitivity")
    rows = []
    for label, ks, vs in [("16B/64B", 16, 64), ("36B/100B", 36, 100),
                           ("36B/512B", 36, 512), ("36B/1024B", 36, 1024),
                           ("128B/100B", 128, 100), ("128B/1024B", 128, 1024)]:
        wa, _ = run(N, ks, vs, Options(), UP, label)
        rows.append((label, wa))
    table(rows)

    # update effect
    print("\n### Effect of updates (36B/100B)")
    rows = []
    for upd in [0.0, 0.1, 0.25, 0.5, 1.0]:
        label = f"{int(upd*100)}% updates"
        wa, _ = run(N, 36, 100, Options(), upd, label)
        rows.append((label, wa))
    table(rows)

    # Final comparison
    wa_default, _ = run(N, 36, 100, Options(), 0.0, "Default config (final)")
    print(f"\n{'=' * 72}")
    print(f"  Comparison ({N:,} keys, 36B keys, 100B values, insert-only)")
    print(f"{'=' * 72}")
    print(f"  | Engine                | Write amplification |")
    print(f"  |----------------------|---------------------|")
    print(f"  | ByteCaskDB direct    | ~1.00x              |")
    print(f"  | UnorderedView        | ~{wa_default:.2f}x              |")
    print(f"  | LMDB (COW B-tree)    | ~5-6x               |")
    print(f"  | RocksDB (LSM)        | ~10-30x             |")


if __name__ == "__main__":
    main()
