# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# The Python reference implementation (reference/bytecask_ref.py): its own
# behaviour, a differential run against the native engine through the same
# API (bytecaskdb.DB), and the file format in both directions.

import errno
import os
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
import bytecask_ref as ref  # noqa: E402
from bytecask_ref import EntryType as T  # noqa: E402


def data_files(path):
    return sorted(Path(path).glob("*.data"))


def entry_types(path):
    (committed, _), = [ref.scan_committed(p.read_bytes()) for p in data_files(path)
                       if p.stat().st_size]
    return [e.type for _, e in committed]


# ---------------------------------------------------------------------------
# The reference on its own
# ---------------------------------------------------------------------------


def test_dict_like_access(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db[b"a"] = b"1"
        assert db[b"a"] == b"1" and b"a" in db and db.get(b"zz", b"none") == b"none"
        del db[b"a"]
        del db[b"a"]  # absent: nothing happens
        assert b"a" not in db
        with pytest.raises(KeyError):
            db[b"a"]


def test_sequences_and_framing(tmp_path):
    with ref.DB.open(tmp_path) as db:
        assert db.put(b"a", b"1").sequence == 1
        with db.batch() as b:
            b[b"b"] = b"2"
            b[b"c"] = b"3"
        assert b.result.sequence == 5  # BULK_BEGIN 2, puts 3-4, BULK_END 5
        assert db.delete(b"zz") is None
        assert db.delete(b"a").sequence == 6
        assert db.delete_range(b"x", b"a") == ref.CommitResult(0, True)
    assert entry_types(tmp_path) == [T.PUT, T.BULK_BEGIN, T.PUT, T.PUT, T.BULK_END, T.DELETE]


def test_scans(tmp_path):
    with ref.DB.open(tmp_path) as db:
        for k in [b"a", b"b1", b"b2", b"b\xff", b"c"]:
            db.put(k, k.upper(), sync=False)
        assert list(db.keys(b"b")) == [b"b1", b"b2", b"b\xff", b"c"]
        assert list(db.rkeys(b"b2")) == [b"b2", b"b1", b"a"]
        assert list(db.rkeys()) == [b"c", b"b\xff", b"b2", b"b1", b"a"]
        assert list(db.prefix(b"b")) == [(b"b1", b"B1"), (b"b2", b"B2"), (b"b\xff", b"B\xff")]
        assert [k for k, _ in db.rprefix(b"b")] == [b"b\xff", b"b2", b"b1"]


def test_reopen_replays_every_write(tmp_path):
    with ref.DB.open(tmp_path, max_file_bytes=200) as db:
        for i in range(50):
            db.put(f"k{i:02}".encode(), f"v{i}".encode(), sync=False)
        db.delete_range(b"k10", b"k20")
        del db[b"k30"]
    assert len(data_files(tmp_path)) > 5
    with ref.DB.open(tmp_path, max_file_bytes=200) as db:
        expected = [f"k{i:02}".encode() for i in range(50) if not 10 <= i < 20 and i != 30]
        assert list(db.keys()) == expected
        assert db[b"k05"] == b"v5"
        assert db.put(b"n", b"v").sequence == 53


def test_torn_batch_in_newest_file_is_cut(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db[b"a"] = b"1"
        with db.batch() as b:
            b[b"b"] = b"2"
            b[b"c"] = b"3"
    path, = [p for p in data_files(tmp_path) if p.stat().st_size]
    with open(path, "r+b") as f:
        f.truncate(path.stat().st_size - 5)  # BULK_END torn
    with ref.DB.open(tmp_path) as db:
        assert list(db.keys()) == [b"a"]
    assert path.stat().st_size == len(ref.Entry(1, T.PUT, b"a", b"1").encode())


def test_damage_in_an_older_file_is_refused(tmp_path):
    with ref.DB.open(tmp_path, max_file_bytes=1) as db:  # a file per write
        db[b"a"] = b"1"
        db[b"b"] = b"2"
    # Names order files only by the second, so find the first by its sequence.
    oldest, = [p for p in data_files(tmp_path)
               if (c := ref.scan_committed(p.read_bytes())[0]) and c[0][1].sequence == 1]
    raw = bytearray(oldest.read_bytes())
    raw[-1] ^= 0xFF
    oldest.write_bytes(bytes(raw))
    with pytest.raises(RuntimeError, match="corrupt data file"):
        ref.DB.open(tmp_path, max_file_bytes=1)


def test_a_failed_sync_stops_writes(tmp_path, monkeypatch):
    db = ref.DB.open(tmp_path)
    db[b"a"] = b"1"
    real = ref._datasync

    def fail_once(fd):
        monkeypatch.setattr(ref, "_datasync", real)
        raise OSError(errno.EIO, "injected")

    monkeypatch.setattr(ref, "_datasync", fail_once)
    with pytest.raises(OSError):
        db[b"b"] = b"2"
    # A sync that succeeds now may not write what the failed one left behind.
    with pytest.raises(ref.DbDegraded):
        db.put(b"c", b"3", sync=False)
    assert db.is_degraded and "injected" in db.degraded_reason
    assert db[b"a"] == b"1"  # reads go on
    with pytest.raises(ref.DbDegraded):
        db.close()
    with ref.DB.open(tmp_path) as db:  # the failed write may be there or not, never in part
        assert db[b"a"] == b"1" and db.get(b"b") in (None, b"2") and b"c" not in db
        db[b"c"] = b"3"


def test_a_torn_write_is_cut_at_reopen(tmp_path, monkeypatch):
    db = ref.DB.open(tmp_path, max_file_bytes=300)
    db[b"a"] = b"1"

    def write_half(fd, data, offset):
        os.pwrite(fd, data[: len(data) // 2], offset)
        raise OSError(errno.ENOSPC, "injected")

    with monkeypatch.context() as m:
        m.setattr(ref, "_pwrite_all", write_half)
        with pytest.raises(OSError):
            with db.batch() as b:
                b[b"b"] = b"2" * 50
                b[b"c"] = b"3" * 50
    with pytest.raises(ref.DbDegraded):
        db.close()
    # Reopened, the torn bytes are gone: later writes rotate the file away,
    # and a tail left in it would make the next open refuse it as damage.
    with ref.DB.open(tmp_path, max_file_bytes=300) as db:
        assert list(db.keys()) == [b"a"]
        for i in range(20):
            db[f"k{i}".encode()] = b"x" * 40
    with ref.DB.open(tmp_path, max_file_bytes=300) as db:
        assert len(list(db.keys())) == 21


def test_open_makes_the_newest_file_durable(tmp_path, monkeypatch):
    with ref.DB.open(tmp_path, max_file_bytes=1) as db:  # a file per write
        db[b"a"] = b"1"
        db[b"b"] = b"2"
    newest, = [p for p in data_files(tmp_path)
               if (c := ref.scan_committed(p.read_bytes())[0]) and c[0][1].sequence == 2]
    synced = []
    real = ref._fullsync

    def record(fd):
        synced.append(os.fstat(fd).st_ino)
        real(fd)

    monkeypatch.setattr(ref, "_fullsync", record)
    ref.DB.open(tmp_path, max_file_bytes=1).close()
    assert newest.stat().st_ino in synced


def test_transactions(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db[b"price"] = b"10"
        db[b"r1"] = b"x"

        with db.transaction() as txn:
            txn[b"order"] = txn[b"price"]
            assert txn[b"order"] == b"10"  # reads its own writes
            txn.delete_range(b"a", b"p")
            assert b"order" not in txn and txn.get(b"price") == b"10"
        assert b"order" not in db

        with pytest.raises(ref.ConflictError):
            with db.transaction() as txn:
                txn.ensure_unchanged(b"price")
                db[b"price"] = b"20"
                txn[b"order"] = b"1"
        assert b"order" not in db

        with pytest.raises(ref.ConflictError):
            with db.transaction() as txn:  # writes a key changed since it began
                db[b"price"] = b"30"
                txn[b"price"] = b"40"

        with pytest.raises(ref.ConflictError):
            with db.transaction() as txn:
                txn.ensure_range_unchanged(b"r", b"s")
                del db[b"r1"]

        with pytest.raises(ValueError):
            with db.transaction() as txn:
                txn.ensure_present(b"k")
                txn.ensure_absent(b"k")

        with pytest.raises(RuntimeError, match="in the block"):
            with db.transaction() as txn:
                txn[b"never"] = b"1"
                raise RuntimeError("in the block")
        assert b"never" not in db


# ---------------------------------------------------------------------------
# Against the native engine, through the same API
# ---------------------------------------------------------------------------


@pytest.fixture
def bc():
    return pytest.importorskip("bytecaskdb")


def assert_same_reads(r, n, rng, keys, where):
    for k in rng.sample(keys, 5):
        assert r.get(k) == n.get(k), (where, k)
        assert (k in r) == (k in n), (where, k)
    start = rng.choice([*keys, b""])
    pfx = start[:1]
    for scan in ("items", "keys", "ritems", "rkeys"):
        assert list(getattr(r, scan)(start)) == list(getattr(n, scan)(start)), (where, scan, start)
    assert list(r.prefix(pfx)) == list(n.prefix(pfx)), (where, pfx)
    assert list(r.rprefix(pfx)) == list(n.rprefix(pfx)), (where, pfx)


@pytest.mark.parametrize("seed", range(6))
def test_differential_against_native(tmp_path, bc, seed):
    """One seeded workload on both engines: every commit, conflict, sequence
    and read must agree, through rotation, reopen, transactions held open
    across other writes, and native vacuum (which the reference does not have
    and must not need)."""
    rng = random.Random(seed)
    keys = sorted({bytes(rng.choice(b"abc") for _ in range(rng.randint(1, 4)))
                   for _ in range(60)})
    max_file_bytes = 512

    def open_both():
        return (ref.DB.open(tmp_path / "ref", max_file_bytes=max_file_bytes),
                bc.DB.open(str(tmp_path / "native"), max_file_bytes=max_file_bytes))

    r, n = open_both()
    open_txns = []  # (reference, native) transactions begun and not yet committed
    snaps = []

    def value():
        return bytes(rng.randrange(256) for _ in range(rng.randint(0, 40)))

    def key_range():
        a, b = rng.choice(keys), rng.choice(keys)
        return min(a, b), max(a, b)

    def stage(rb, nb, guards):
        for _ in range(rng.randint(0, 4)):
            dice = rng.random()
            if dice < 0.6:
                k, v = rng.choice(keys), value()
                rb[k] = v
                nb[k] = v
            elif dice < 0.9:
                k = rng.choice(keys)
                del rb[k]
                del nb[k]
            else:
                a, b = key_range()
                rb.delete_range(a, b)
                nb.delete_range(a, b)
        if not guards:
            return
        for k in rng.sample(keys, rng.randint(0, 2)):
            guard = rng.choice(["present", "absent", "unchanged", "unchanged"])
            getattr(rb, f"ensure_{guard}")(k)
            getattr(nb, f"ensure_{guard}")(k)
        if rng.random() < 0.3:
            a, b = key_range()
            rb.ensure_range_unchanged(a, b)
            nb.ensure_range_unchanged(a, b)

    def finish(r_ctx, n_ctx):
        """Exits both contexts: the commit sequence, or "conflict"."""
        outcomes = []
        for ctx, txn in (r_ctx, n_ctx):
            try:
                ctx.__exit__(None, None, None)
                outcomes.append(txn.result.sequence)
            except (ref.ConflictError, bc.ConflictError):
                outcomes.append("conflict")
        return outcomes

    for step in range(1500):
        dice = rng.random()
        sync = rng.random() < 0.1
        where = f"seed {seed} step {step}"
        if dice < 0.30:
            k, v = rng.choice(keys), value()
            assert r.put(k, v, sync=sync).sequence == n.put(k, v, sync=sync).sequence, where
        elif dice < 0.40:
            k = rng.choice(keys)
            rr, nr = r.delete(k, sync=sync), n.delete(k, sync=sync)
            assert (rr and rr.sequence) == (nr and nr.sequence), where
        elif dice < 0.45:
            a, b = key_range()
            assert (r.delete_range(a, b, sync=sync).sequence
                    == n.delete_range(a, b, sync=sync).sequence), where
        elif dice < 0.55:
            r_ctx, n_ctx = r.batch(sync=sync), n.batch(sync=sync)
            stage(r_ctx.__enter__(), n_ctx.__enter__(), guards=False)
            outcome = finish((r_ctx, r_ctx._batch), (n_ctx, n_ctx._batch))
            assert outcome[0] == outcome[1], where
        elif dice < 0.62:
            r_ctx, n_ctx = r.transaction(sync=sync), n.transaction(sync=sync)
            open_txns.append((r_ctx, r_ctx.__enter__(), n_ctx, n_ctx.__enter__()))
        elif dice < 0.72 and open_txns:
            r_ctx, r_txn, n_ctx, n_txn = open_txns.pop(rng.randrange(len(open_txns)))
            stage(r_txn, n_txn, guards=True)
            for k in rng.sample(keys, 3):  # reads see the transaction's own writes
                assert r_txn.get(k) == n_txn.get(k), (where, k)
                assert (k in r_txn) == (k in n_txn), (where, k)
            outcome = finish((r_ctx, r_txn), (n_ctx, n_txn))
            assert outcome[0] == outcome[1], where
        elif dice < 0.78:
            snaps.append((r.snapshot(), n.snapshot()))
            if len(snaps) > 8:
                snaps.pop(0)
        elif dice < 0.94:
            assert_same_reads(r, n, rng, keys, where)
            if snaps:
                rs, ns = rng.choice(snaps)
                assert_same_reads(rs, ns, rng, keys, where + " (snapshot)")
        elif dice < 0.97:
            n.vacuum()
        else:
            open_txns.clear()
            snaps.clear()
            r.close()
            n.close()
            r, n = open_both()
    assert list(r.items()) == list(n.items())
    r.close()
    n.close()


def fill(put, rng):
    for _ in range(300):
        put(f"k{rng.randrange(100):03}".encode(),
            bytes(rng.randrange(256) for _ in range(rng.randint(0, 30))))


def test_native_opens_reference_files(tmp_path, bc):
    rng = random.Random(1)
    with ref.DB.open(tmp_path, max_file_bytes=1024) as r:
        fill(lambda k, v: r.put(k, v, sync=False), rng)
        with r.batch() as b:
            b.delete_range(b"k010", b"k020")
            del b[b"k050"]
        expected = list(r.items())
    with bc.DB.open(str(tmp_path), max_file_bytes=1024) as n:
        assert list(n.items()) == expected


def test_reference_opens_native_files(tmp_path, bc):
    rng = random.Random(2)
    with bc.DB.open(str(tmp_path), max_file_bytes=1024) as n:
        fill(lambda k, v: n.put(k, v, sync=False), rng)
        with n.batch() as b:
            b.delete_range(b"k010", b"k020")
            del b[b"k050"]
        fill(lambda k, v: n.put(k, v, sync=False), rng)
        while n.vacuum():
            pass
        expected = list(n.items())
    with ref.DB.open(tmp_path, max_file_bytes=1024) as r:
        assert list(r.items()) == expected
