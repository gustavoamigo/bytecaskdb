# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Property-based validation of the independence assumption.
#
# The proof matrix uses fixed symbolic values ("new0", "v0") across all
# 800 cells.  These tests validate: for a given plan shape, the engine
# produces the same structural delta regardless of what concrete bytes
# the keys and values contain.
#
# 8 tests cover the 8 structurally distinct deltas under SUCCESS.
# See docs/correctness_validation.md for the full rationale.

import os
import tempfile

from hypothesis import HealthCheck, assume, given, settings
from hypothesis import strategies as st

import bytecaskdb._bytecaskdb as bc

# ---------------------------------------------------------------------------
# Strategies
# ---------------------------------------------------------------------------

key_bytes = st.one_of(
    st.binary(min_size=1, max_size=1),
    st.binary(min_size=2, max_size=64),
    st.binary(min_size=4000, max_size=4096),
)

value_bytes = st.one_of(
    st.just(b""),
    st.binary(min_size=1, max_size=1),
    st.binary(min_size=2, max_size=64),
    st.binary(min_size=4095, max_size=4097),
    st.binary(min_size=8000, max_size=16384),
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

SETTINGS = settings(
    max_examples=50,
    deadline=None,
    suppress_health_check=[HealthCheck.too_slow, HealthCheck.data_too_large],
)


def sync_opts():
    opts = bc.WriteOptions()
    opts.sync = True
    return opts


def assert_recovery(path, expected_kv, absent_keys):
    db = bc.DB.open(path)
    for k, v in expected_kv.items():
        assert db.get(k) == v, (
            f"recovery: key {k!r} expected {v!r}, got {db.get(k)!r}"
        )
    for k in absent_keys:
        assert db.get(k) is None, f"recovery: key {k!r} should be absent"


# ---------------------------------------------------------------------------
# Delta 1: single_put — 1 added, 1 value, seq_advance=1
# ---------------------------------------------------------------------------


@SETTINGS
@given(k=key_bytes, v=value_bytes)
def test_delta1_single_put(k, v):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        db.put(k, v, sync_opts())

        assert db.get(k) == v
        assert db.current_sequence() - seq_before == 1
        assert not db.is_degraded

        del db
        assert_recovery(path, {k: v}, [])


# ---------------------------------------------------------------------------
# Delta 2: causality_overwrite — 1 added, 1 value, seq_advance=4
# Last-write-wins: two PUTs to the same key in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(k=key_bytes, v0=value_bytes, v1=value_bytes)
def test_delta2_causality_overwrite(k, v0, v1):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        plan = bc.WritePlan()
        plan.put(k, v0)
        plan.put(k, v1)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k) == v1
        assert db.current_sequence() - seq_before == 4
        assert not db.is_degraded

        del db
        assert_recovery(path, {k: v1}, [])


# ---------------------------------------------------------------------------
# Delta 3: causality_put_del_put — 1 added, 1 value, seq_advance=5
# PUT, DELETE, PUT on the same key; final PUT wins.
# ---------------------------------------------------------------------------


@SETTINGS
@given(k=key_bytes, v0=value_bytes, v1=value_bytes)
def test_delta3_causality_put_del_put(k, v0, v1):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        plan = bc.WritePlan()
        plan.put(k, v0)
        plan.del_(k)
        plan.put(k, v1)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k) == v1
        assert db.current_sequence() - seq_before == 5
        assert not db.is_degraded

        del db
        assert_recovery(path, {k: v1}, [])


# ---------------------------------------------------------------------------
# Delta 4: mixed_batch — 1 added + 1 removed, 1 value, seq_advance=4
# PUT a new key + DELETE an existing key in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(
    k_new=key_bytes,
    v_new=value_bytes,
    k_exist=key_bytes,
    v_exist=value_bytes,
)
def test_delta4_mixed_batch(k_new, v_new, k_exist, v_exist):
    assume(k_new != k_exist)
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        db.put(k_exist, v_exist, sync_opts())
        seq_before = db.current_sequence()

        plan = bc.WritePlan()
        plan.put(k_new, v_new)
        plan.del_(k_exist)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k_new) == v_new
        assert db.get(k_exist) is None
        assert db.current_sequence() - seq_before == 4
        assert not db.is_degraded

        del db
        assert_recovery(path, {k_new: v_new}, [k_exist])


# ---------------------------------------------------------------------------
# Delta 5: multi_put — 2 added, 2 values, seq_advance=4
# Two PUTs to different keys in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(k0=key_bytes, v0=value_bytes, k1=key_bytes, v1=value_bytes)
def test_delta5_multi_put(k0, v0, k1, v1):
    assume(k0 != k1)
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        plan = bc.WritePlan()
        plan.put(k0, v0)
        plan.put(k1, v1)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k0) == v0
        assert db.get(k1) == v1
        assert db.current_sequence() - seq_before == 4
        assert not db.is_degraded

        del db
        assert_recovery(path, {k0: v0, k1: v1}, [])


# ---------------------------------------------------------------------------
# Delta 6: large_batch — 3 added, 3 values, seq_advance=5
# Three PUTs to three distinct keys in one batch.
# ---------------------------------------------------------------------------


@SETTINGS
@given(
    k0=key_bytes,
    v0=value_bytes,
    k1=key_bytes,
    v1=value_bytes,
    k2=key_bytes,
    v2=value_bytes,
)
def test_delta6_large_batch(k0, v0, k1, v1, k2, v2):
    assume(len({k0, k1, k2}) == 3)
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        plan = bc.WritePlan()
        plan.put(k0, v0)
        plan.put(k1, v1)
        plan.put(k2, v2)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k0) == v0
        assert db.get(k1) == v1
        assert db.get(k2) == v2
        assert db.current_sequence() - seq_before == 5
        assert not db.is_degraded

        del db
        assert_recovery(path, {k0: v0, k1: v1, k2: v2}, [])


# ---------------------------------------------------------------------------
# Delta 7: single_delete — 0 added, 1 removed, seq_advance=1
# Key-byte independence for the delete path.
# ---------------------------------------------------------------------------


@SETTINGS
@given(k=key_bytes, v=value_bytes)
def test_delta7_single_delete(k, v):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        db.put(k, v, sync_opts())
        seq_before = db.current_sequence()

        assert db.del_(k, sync_opts())

        assert db.get(k) is None
        assert db.current_sequence() - seq_before == 1
        assert not db.is_degraded

        del db
        assert_recovery(path, {}, [k])


# ---------------------------------------------------------------------------
# Delta 8: causality_put_del — 0 added, 1 removed, seq_advance=4
# PUT then DELETE on the same key in one batch; net effect: absent.
# ---------------------------------------------------------------------------


@SETTINGS
@given(k=key_bytes, v=value_bytes)
def test_delta8_causality_put_del(k, v):
    with tempfile.TemporaryDirectory() as tmpdir:
        path = os.path.join(tmpdir, "db")
        db = bc.DB.open(path)

        seq_before = db.current_sequence()
        plan = bc.WritePlan()
        plan.put(k, v)
        plan.del_(k)
        assert db.apply_batch(plan, sync_opts())

        assert db.get(k) is None
        assert db.current_sequence() - seq_before == 4
        assert not db.is_degraded

        del db
        assert_recovery(path, {}, [k])
