# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# The Python reference implementation (reference/bytecask_ref.py): its own
# behaviour, a differential run against the native engine, and the file format
# in both directions.

import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
import bytecask_ref as ref  # noqa: E402

NO_SYNC = ref.WriteOptions(sync=False)


def data_files(path):
    return sorted(Path(path).glob("*.data"))


# ---------------------------------------------------------------------------
# The reference on its own
# ---------------------------------------------------------------------------


def test_crc32c_check_value():
    assert ref.crc32c(b"123456789") == 0xE3069283


def test_tree_is_persistent():
    loc = lambda s: ref.Location(1, 0, s)  # noqa: E731
    roots = [None]
    keys = [b"m", b"c", b"x", b"a", b"e", b"z", b"d"]
    for i, k in enumerate(keys, start=1):
        roots.append(ref.tree_put(roots[-1], k, loc(i)))
    roots.append(ref.tree_erase(roots[-1], b"c"))  # two children
    roots.append(ref.tree_erase(roots[-1], b"m"))  # the root
    roots.append(ref.tree_put(roots[-1], b"x", loc(99)))

    def keys_of(root):
        return [k for k, _ in ref.tree_ascend(root)]

    # Every older root still holds exactly what it held when it was made.
    for n in range(len(keys) + 1):
        assert keys_of(roots[n]) == sorted(keys[:n])
    assert keys_of(roots[-1]) == [b"a", b"d", b"e", b"x", b"z"]
    assert ref.tree_get(roots[-1], b"x").sequence == 99
    assert ref.tree_get(roots[-2], b"x").sequence == 3
    assert [k for k, _ in ref.tree_descend(roots[-1], b"w")] == [b"e", b"d", b"a"]
    assert [k for k, _ in ref.tree_ascend(roots[-1], b"b")] == [b"d", b"e", b"x", b"z"]


def test_sequences_and_framing(tmp_path):
    with ref.DB.open(tmp_path) as db:
        assert db.put(b"a", b"1").sequence == 1
        plan = ref.WritePlan()
        plan.put(b"b", b"2")
        plan.put(b"c", b"3")
        assert db.apply_batch(plan).sequence == 5  # BulkBegin 2, puts 3-4, BulkEnd 5
        assert db.del_(b"zz") is None
        assert db.del_(b"a").sequence == 6
        assert db.del_range(b"x", b"a") == ref.CommitResult(0, True)
    (entries, _), = [ref.scan_committed(p.read_bytes()) for p in data_files(tmp_path)]
    assert [e.type for _, e in entries] == [
        ref.EntryType.Put, ref.EntryType.BulkBegin, ref.EntryType.Put,
        ref.EntryType.Put, ref.EntryType.BulkEnd, ref.EntryType.Delete]


def test_reopen_replays_every_write(tmp_path):
    opts = ref.Options(max_file_bytes=200)
    with ref.DB.open(tmp_path, opts) as db:
        for i in range(50):
            db.put(f"k{i:02}".encode(), f"v{i}".encode(), NO_SYNC)
        db.del_range(b"k10", b"k20")
        db.del_(b"k30")
    assert len(data_files(tmp_path)) > 5
    with ref.DB.open(tmp_path, opts) as db:
        expected = [f"k{i:02}".encode() for i in range(50) if not 10 <= i < 20 and i != 30]
        assert list(db.keys_from()) == expected
        assert db.get(b"k05") == b"v5"
        assert db.put(b"n", b"v").sequence == 53


def test_torn_batch_in_newest_file_is_cut(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db.put(b"a", b"1")
        plan = ref.WritePlan()
        plan.put(b"b", b"2")
        plan.put(b"c", b"3")
        db.apply_batch(plan)
    path = data_files(tmp_path)[0]
    whole = path.stat().st_size
    with open(path, "r+b") as f:
        f.truncate(whole - 5)  # BulkEnd torn
    with ref.DB.open(tmp_path) as db:
        assert list(db.keys_from()) == [b"a"]
    assert path.stat().st_size == len(ref.encode_entry(ref.Entry(1, ref.EntryType.Put, b"a", b"1")))


def test_damage_in_an_older_file_is_refused(tmp_path):
    opts = ref.Options(max_file_bytes=1)  # a file per write
    with ref.DB.open(tmp_path, opts) as db:
        db.put(b"a", b"1")
        db.put(b"b", b"2")
    # Names order files only by the second, so find the first by its sequence.
    oldest, = [p for p in data_files(tmp_path)
               if (s := ref.scan_committed(p.read_bytes())[0]) and s[0][1].sequence == 1]
    raw = bytearray(oldest.read_bytes())
    raw[-1] ^= 0xFF
    oldest.write_bytes(bytes(raw))
    with pytest.raises(RuntimeError, match="corrupt data file"):
        ref.DB.open(tmp_path, opts)


def test_guards(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db.put(b"price", b"10")
        db.put(b"r1", b"x")
        snap = db.snapshot()
        db.put(b"price", b"20")

        plan = ref.WritePlan(snap)
        plan.ensure_unchanged(b"price")
        plan.put(b"order", b"1")
        assert db.apply_batch(plan) is None

        plan = ref.WritePlan(snap)  # writes a key changed since the snapshot
        plan.put(b"price", b"30")
        assert db.apply_batch(plan) is None

        plan = ref.WritePlan(snap)
        plan.ensure_range_unchanged(b"r", b"s")
        plan.put(b"order", b"1")
        assert db.apply_batch(plan) is not None
        db.del_(b"r1")
        plan = ref.WritePlan(snap)
        plan.ensure_range_unchanged(b"r", b"s")
        assert db.apply_batch(plan) is None

        assert snap.get(b"price") == b"10" and db.get(b"price") == b"20"
        with pytest.raises(ValueError):
            ref.WritePlan().ensure_unchanged(b"k")


# ---------------------------------------------------------------------------
# Against the native engine
# ---------------------------------------------------------------------------

@pytest.fixture
def bc():
    return pytest.importorskip("bytecaskdb._bytecaskdb")


def native_options(bc, max_file_bytes):
    opts = bc.Options()
    opts.max_file_bytes = max_file_bytes
    return opts


def native_write_options(bc, sync):
    opts = bc.WriteOptions()
    opts.sync = sync
    return opts


def commit(result):
    return None if result is None else result.sequence


def assert_same_reads(r, n, rng, keys, where):
    for k in rng.sample(keys, 5):
        assert r.get(k) == n.get(k), (where, k)
        assert r.contains_key(k) == n.contains_key(k), (where, k)
    start = rng.choice(keys + [b""])
    assert list(r.iter_from(start)) == list(n.iter_from(start)), (where, start)
    assert list(r.keys_from(start)) == list(n.keys_from(start)), (where, start)
    assert list(r.riter_from(start)) == list(n.riter_from(start)), (where, start)
    assert list(r.rkeys_from(start)) == list(n.rkeys_from(start)), (where, start)


@pytest.mark.parametrize("seed", range(6))
def test_differential_against_native(tmp_path, bc, seed):
    """One seeded workload on both engines: every commit, conflict, sequence
    and read must agree, through rotation, reopen, held snapshots and native
    vacuum (which the reference does not have and must not need)."""
    rng = random.Random(seed)
    keys = sorted({bytes(rng.choice(b"abc") for _ in range(rng.randint(1, 4))) for _ in range(60)})
    max_file_bytes = 512
    r = ref.DB.open(tmp_path / "ref", ref.Options(max_file_bytes=max_file_bytes))
    n = bc.DB.open(str(tmp_path / "native"), native_options(bc, max_file_bytes))
    snaps = []  # (reference, native), consumed by plans like native ones are

    def value():
        return bytes(rng.randrange(256) for _ in range(rng.randint(0, 40)))

    def key_range():
        a, b = rng.choice(keys), rng.choice(keys)
        return min(a, b), max(a, b)

    for step in range(1500):
        dice = rng.random()
        sync = rng.random() < 0.1
        ro, no = ref.WriteOptions(sync=sync), native_write_options(bc, sync)
        where = f"seed {seed} step {step}"
        if dice < 0.30:
            k, v = rng.choice(keys), value()
            assert commit(r.put(k, v, ro)) == commit(n.put(k, v, no)), where
        elif dice < 0.40:
            k = rng.choice(keys)
            assert commit(r.del_(k, ro)) == commit(n.del_(k, no)), where
        elif dice < 0.45:
            a, b = key_range()
            assert commit(r.del_range(a, b, ro)) == commit(n.del_range(a, b, no)), where
        elif dice < 0.65:
            pair = snaps.pop(rng.randrange(len(snaps))) if snaps and rng.random() < 0.6 else None
            rp = ref.WritePlan(pair[0]) if pair else ref.WritePlan()
            np_ = bc.WritePlan(pair[1]) if pair else bc.WritePlan()
            for _ in range(rng.randint(0, 4)):
                kind = rng.random()
                if kind < 0.6:
                    k, v = rng.choice(keys), value()
                    rp.put(k, v), np_.put(k, v)
                elif kind < 0.9:
                    k = rng.choice(keys)
                    rp.del_(k), np_.del_(k)
                else:
                    a, b = key_range()
                    rp.del_range(a, b), np_.del_range(a, b)
            guarded = rng.sample(keys, rng.randint(0, 2))
            for k in guarded:
                kinds = ["present", "absent"] + (["unchanged"] * 2 if pair else [])
                kind = rng.choice(kinds)
                getattr(rp, f"ensure_{kind}")(k), getattr(np_, f"ensure_{kind}")(k)
            if pair and rng.random() < 0.3:
                a, b = key_range()
                rp.ensure_range_unchanged(a, b), np_.ensure_range_unchanged(a, b)
            assert commit(r.apply_batch(rp, ro)) == commit(n.apply_batch(np_, no)), where
        elif dice < 0.75:
            snaps.append((r.snapshot(), n.snapshot()))
            if len(snaps) > 8:
                snaps.pop(0)
        elif dice < 0.93:
            assert_same_reads(r, n, rng, keys, where)
            if snaps:
                rs, ns = rng.choice(snaps)
                assert_same_reads(rs, ns, rng, keys, where + " (snapshot)")
        elif dice < 0.96:
            n.vacuum()
        else:
            snaps.clear()
            r.close(), n.close()
            r = ref.DB.open(tmp_path / "ref", ref.Options(max_file_bytes=max_file_bytes))
            n = bc.DB.open(str(tmp_path / "native"), native_options(bc, max_file_bytes))
    assert list(r.iter_from()) == list(n.iter_from())
    r.close(), n.close()


def fill(put, rng):
    for i in range(300):
        put(f"k{rng.randrange(100):03}".encode(), bytes(rng.randrange(256) for _ in range(rng.randint(0, 30))))


def test_native_opens_reference_files(tmp_path, bc):
    rng = random.Random(1)
    with ref.DB.open(tmp_path, ref.Options(max_file_bytes=1024)) as r:
        fill(lambda k, v: r.put(k, v, NO_SYNC), rng)
        plan = ref.WritePlan()
        plan.del_range(b"k010", b"k020")
        plan.del_(b"k050")
        r.apply_batch(plan)
        expected = list(r.iter_from())
    n = bc.DB.open(str(tmp_path), native_options(bc, 1024))
    assert list(n.iter_from()) == expected
    n.close()


def test_reference_opens_native_files(tmp_path, bc):
    rng = random.Random(2)
    n = bc.DB.open(str(tmp_path), native_options(bc, 1024))
    fill(lambda k, v: n.put(k, v, native_write_options(bc, False)), rng)
    plan = bc.WritePlan()
    plan.del_range(b"k010", b"k020")
    plan.del_(b"k050")
    n.apply_batch(plan)
    fill(lambda k, v: n.put(k, v, native_write_options(bc, False)), rng)
    while n.vacuum():
        pass
    expected = list(n.iter_from())
    n.close()
    with ref.DB.open(tmp_path, ref.Options(max_file_bytes=1024)) as r:
        assert list(r.iter_from()) == expected
