# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Property-based validation of the independence assumption.
#
# The proof matrix uses fixed symbolic keys and values ("k0", "new0") in
# every cell.  These tests validate: for a given plan shape, the engine
# produces the same structural delta whatever bytes the keys and values
# contain, whatever keys are already there, and however the DB is opened.
#
# One test per structurally distinct delta of a transition that lands
# (SUCCESS).  See docs/correctness_validation.md for the full rationale.

import itertools
import os
import tempfile
from dataclasses import dataclass

from hypothesis import HealthCheck, assume, given, settings
from hypothesis import strategies as st

import bytecaskdb._bytecaskdb as bc

MAX_KEY_BYTES = 4096
MAX_VALUE_BYTES = 4 * 1024 * 1024
SMALL_FILE_BYTES = 64 * 1024

# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

# Keys of one test share a prefix and differ in a short suffix over this
# alphabet, so they are prefixes of one another, differ in one bit (0x00 and
# 0x01, 0x00 and 0x80) and crowd the same leaves.  The key directory keeps no
# key bytes, only a crit bit and a fingerprint per key: uniformly random keys
# would diverge in their first byte and tell it nothing.
ALPHABET = (0x00, 0x01, 0x80, 0xFF)
SUFFIX_BYTES = 6

suffixes = st.lists(st.sampled_from(ALPHABET), max_size=SUFFIX_BYTES).map(bytes)

prefixes = st.one_of(
    st.just(b""),
    st.binary(min_size=1, max_size=32),
    # Keys within a few bytes of max_key_bytes.
    st.binary(
        min_size=MAX_KEY_BYTES - SUFFIX_BYTES - 10,
        max_size=MAX_KEY_BYTES - SUFFIX_BYTES,
    ),
)

random_keys = st.binary(max_size=64)


def _sized(seed: bytes, size: int) -> bytes:
    return (seed * (size // len(seed) + 1))[:size]


value_bytes = st.one_of(
    st.just(b""),
    st.binary(min_size=1, max_size=1),
    st.binary(min_size=2, max_size=64),
    st.binary(min_size=4095, max_size=4097),
    st.binary(min_size=8000, max_size=16384),
    # At and just under max_value_bytes; larger than SMALL_FILE_BYTES.
    st.builds(
        _sized,
        st.binary(min_size=1, max_size=16),
        st.sampled_from([MAX_VALUE_BYTES - 1, MAX_VALUE_BYTES]),
    ),
)

# Every suffix of up to four bytes: 341 keys, several leaves' worth.
CROWD = [
    bytes(t)
    for n in range(5)
    for t in itertools.product(ALPHABET, repeat=n)
]

# (io_backend, max_file_bytes).  A small file makes the setup rotate, so the
# transition lands on a DB of several sealed files and recovery reads hints.
ENVIRONMENTS = [
    ("Pread", None),
    ("Pread", SMALL_FILE_BYTES),
    ("Mmap", None),
    ("Mmap", SMALL_FILE_BYTES),
    ("BufferPool", SMALL_FILE_BYTES),
]


@dataclass
class World:
    """The DB a transition starts from: its keys, and how it is opened."""

    prefix: bytes
    existing: dict
    io_backend: str
    max_file_bytes: int | None

    def options(self, recovery_threads: int = 4):
        opts = bc.Options()
        opts.recovery_threads = recovery_threads
        opts.io_backend = getattr(bc.IoBackend, self.io_backend)
        if self.max_file_bytes is not None:
            opts.max_file_bytes = self.max_file_bytes
        if self.io_backend == "BufferPool":
            pool = opts.buffer_pool
            pool.capacity_bytes = 16 * self.max_file_bytes
            opts.buffer_pool = pool
        return opts

    def keys(self):
        """A key near the existing ones, or anywhere."""
        return st.one_of(suffixes.map(lambda s: self.prefix + s), random_keys)

    def fresh_keys(self):
        return self.keys().filter(lambda k: k not in self.existing)


@st.composite
def worlds(draw):
    prefix = draw(prefixes)
    population = draw(st.sampled_from(["empty", "sparse", "crowded"]))
    if population == "empty":
        tails = []
    elif population == "sparse":
        tails = sorted(draw(st.sets(suffixes, max_size=12)))
    else:
        # All of CROWD but one residue class, so some short keys stay free.
        modulus = draw(st.integers(min_value=2, max_value=7))
        residue = draw(st.integers(min_value=0, max_value=modulus - 1))
        tails = [t for i, t in enumerate(CROWD) if i % modulus != residue]
    seed = draw(st.binary(max_size=24))
    existing = {
        prefix + t: (seed + bytes([i % 251])) * (i % 4)
        for i, t in enumerate(tails)
    }
    if tails:
        for k in draw(st.sets(random_keys, max_size=3)):
            existing.setdefault(k, seed)
    io_backend, max_file_bytes = draw(st.sampled_from(ENVIRONMENTS))
    return World(prefix, existing, io_backend, max_file_bytes)


def world_with_key(data):
    """A world and one key that exists in it, with whatever neighbours."""
    world = data.draw(worlds())
    k = data.draw(world.keys())
    world.existing[k] = data.draw(value_bytes)
    return world, k


def range_around(data, world):
    """Bounds from < to and a key in [from, to)."""
    a, b, c = sorted(data.draw(world.keys()) for _ in range(3))
    assume(b < c)
    return a, c, b


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

SETTINGS = settings(
    max_examples=50,
    deadline=None,
    suppress_health_check=[
        HealthCheck.too_slow,
        HealthCheck.data_too_large,
        HealthCheck.filter_too_much,
    ],
)

SETUP_BATCH_KEYS = 50


def sync_opts():
    opts = bc.WriteOptions()
    opts.sync = True
    return opts


def apply_to_model(model, ops):
    """The reference: what the ops, in order, do to a dict."""
    for op in ops:
        if op[0] == "put":
            model[op[1]] = op[2]
        elif op[0] == "del":
            model.pop(op[1], None)
        else:
            for k in [k for k in model if op[1] <= k < op[2]]:
                del model[k]


def apply_to_db(db, ops):
    """A single op goes through its own call, several through one batch."""
    if len(ops) == 1:
        op = ops[0]
        if op[0] == "put":
            assert db.put(op[1], op[2], sync_opts()).durable
        elif op[0] == "del":
            assert db.del_(op[1], sync_opts()) is not None
        else:
            assert db.del_range(op[1], op[2], sync_opts()).durable
        return
    plan = bc.WritePlan()
    for op in ops:
        if op[0] == "put":
            plan.put(op[1], op[2])
        elif op[0] == "del":
            plan.del_(op[1])
        else:
            plan.del_range(op[1], op[2])
    assert db.apply_batch(plan, sync_opts())


def assert_state(db, model, touched, when):
    """The DB holds the model and nothing else, by every read path."""
    expected = sorted(model.items())
    assert list(db.iter_from()) == expected, f"{when}: iter_from"
    assert list(db.riter_from(b"\xff" * (MAX_KEY_BYTES + 1))) == expected[::-1], (
        f"{when}: riter_from"
    )
    assert list(db.keys_from()) == [k for k, _ in expected], f"{when}: keys_from"
    for k, v in expected:
        assert db.get(k) == v, f"{when}: get {k!r}"
    for k in touched:
        if k not in model:
            assert db.get(k) is None, f"{when}: {k!r} should be absent"
            assert not db.contains_key(k), f"{when}: {k!r} should be absent"


def check(world, ops):
    """Runs the transition on the world and holds the result to the model,
    in the live DB and after serial and parallel recovery."""
    touched = [k for op in ops for k in op[1 : 2 if op[0] != "range" else 3]]
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        model = dict(world.existing)
        with bc.DB.open(path, world.options()) as db:
            items = sorted(model.items())
            for i in range(0, len(items), SETUP_BATCH_KEYS):
                plan = bc.WritePlan()
                for k, v in items[i : i + SETUP_BATCH_KEYS]:
                    plan.put(k, v)
                assert db.apply_batch(plan, sync_opts())
            seq_before = db.durable_sequence()

            apply_to_db(db, ops)
            apply_to_model(model, ops)

            n = len(ops)
            assert db.durable_sequence() - seq_before == n + (2 if n > 1 else 0)
            assert not db.is_degraded
            assert_state(db, model, touched, "live")

        for threads in (1, 4):
            with bc.DB.open(path, world.options(recovery_threads=threads)) as db:
                assert_state(db, model, touched, f"recovery x{threads}")


# ---------------------------------------------------------------------------
# Delta 1: single_put — 1 added, 1 value, seq_advance=1
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta1_single_put(data):
    world = data.draw(worlds())
    k = data.draw(world.fresh_keys())
    check(world, [("put", k, data.draw(value_bytes))])


# ---------------------------------------------------------------------------
# Delta 2: causality_overwrite — 1 added, 1 value, seq_advance=4
# Last-write-wins: two PUTs to the same key in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta2_causality_overwrite(data):
    world = data.draw(worlds())
    k = data.draw(world.fresh_keys())
    check(
        world,
        [("put", k, data.draw(value_bytes)), ("put", k, data.draw(value_bytes))],
    )


# ---------------------------------------------------------------------------
# Delta 3: causality_put_del_put — 1 added, 1 value, seq_advance=5
# PUT, DELETE, PUT on the same key; final PUT wins.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta3_causality_put_del_put(data):
    world = data.draw(worlds())
    k = data.draw(world.fresh_keys())
    check(
        world,
        [
            ("put", k, data.draw(value_bytes)),
            ("del", k),
            ("put", k, data.draw(value_bytes)),
        ],
    )


# ---------------------------------------------------------------------------
# Delta 4: mixed_batch — 1 added + 1 removed, 1 value, seq_advance=4
# PUT a new key + DELETE an existing key in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta4_mixed_batch(data):
    world, k_exist = world_with_key(data)
    k_new = data.draw(world.fresh_keys())
    check(world, [("put", k_new, data.draw(value_bytes)), ("del", k_exist)])


# ---------------------------------------------------------------------------
# Delta 5: multi_put — 2 added, 2 values, seq_advance=4
# Two PUTs to different keys in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta5_multi_put(data):
    world = data.draw(worlds())
    k0 = data.draw(world.fresh_keys())
    k1 = data.draw(world.fresh_keys())
    assume(k0 != k1)
    check(
        world,
        [("put", k0, data.draw(value_bytes)), ("put", k1, data.draw(value_bytes))],
    )


# ---------------------------------------------------------------------------
# Delta 6: large_batch — 3 added, 3 values, seq_advance=5
# Three PUTs to three distinct keys in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta6_large_batch(data):
    world = data.draw(worlds())
    keys = [data.draw(world.fresh_keys()) for _ in range(3)]
    assume(len(set(keys)) == 3)
    check(world, [("put", k, data.draw(value_bytes)) for k in keys])


# ---------------------------------------------------------------------------
# Delta 7: single_delete — 0 added, 1 removed, seq_advance=1
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta7_single_delete(data):
    world, k = world_with_key(data)
    check(world, [("del", k)])


# ---------------------------------------------------------------------------
# Delta 8: causality_put_del — 0 added, 1 removed, seq_advance=4
# PUT then DELETE on the same key in one batch; net effect: absent.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta8_causality_put_del(data):
    world = data.draw(worlds())
    k = data.draw(world.fresh_keys())
    check(world, [("put", k, data.draw(value_bytes)), ("del", k)])


# ---------------------------------------------------------------------------
# Delta 9: sequential_overwrite — 1 changed, 1 value, seq_advance=1
# PUT to a key that already exists.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta9_sequential_overwrite(data):
    world, k = world_with_key(data)
    check(world, [("put", k, data.draw(value_bytes))])


# ---------------------------------------------------------------------------
# Delta 10: causality_del_put — 1 changed, 1 value, seq_advance=4
# DELETE then PUT on an existing key in one batch; the PUT wins.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta10_causality_del_put(data):
    world, k = world_with_key(data)
    check(world, [("del", k), ("put", k, data.draw(value_bytes))])


# ---------------------------------------------------------------------------
# Delta 11: range_del — every key in [from, to) removed, seq_advance=1
# Which keys those are is decided by key bytes alone.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta11_range_del(data):
    world = data.draw(worlds())
    lo, hi, _ = range_around(data, world)
    check(world, [("range", lo, hi)])


# ---------------------------------------------------------------------------
# Delta 12: range_del_then_put — range removed, 1 key put back, seq_advance=4
# The PUT lands inside the range and after it, so it survives.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta12_range_del_then_put(data):
    world = data.draw(worlds())
    lo, hi, k = range_around(data, world)
    check(world, [("range", lo, hi), ("put", k, data.draw(value_bytes))])


# ---------------------------------------------------------------------------
# Delta 13: put_then_range_del — range removed with the PUT, seq_advance=4
# The PUT lands inside the range and before it, so it does not survive.
# ---------------------------------------------------------------------------


@SETTINGS
@given(data=st.data())
def test_delta13_put_then_range_del(data):
    world = data.draw(worlds())
    lo, hi, k = range_around(data, world)
    check(world, [("put", k, data.draw(value_bytes)), ("range", lo, hi)])
