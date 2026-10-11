# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# The Python reference implementation (reference/bytecask_ref.py): its own
# behaviour, a differential run against the native engine at the engine's
# interface, the Pythonic wrapper (bytecaskdb/ext.py) on both, and the file
# format in both directions.

import errno
import os
import random
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
import bytecask_ref as ref  # noqa: E402
from bytecask_ref import EntryType as T  # noqa: E402

NO_SYNC = ref.WriteOptions(sync=False)


def data_files(path):
    return sorted(Path(path).glob("*.data"))


def file_starting_at(path, sequence):
    """Names order files only by the second, so a file is found by its first sequence."""
    found, = [p for p in data_files(path)
              if (c := ref.scan_committed(p.read_bytes())[0]) and c[0].entry.sequence == sequence]
    return found


def batch(*writes, snapshot=None):
    plan = ref.WritePlan(snapshot)
    for key, value in writes:
        plan.put(key, value)
    return plan


# ---------------------------------------------------------------------------
# The reference on its own
# ---------------------------------------------------------------------------


def test_sequences_framing_and_dump(tmp_path, capsys):
    with ref.DB.open(tmp_path) as db:
        assert db.put(b"a", b"1").sequence == 1
        assert db.apply_batch(batch((b"b", b"2"), (b"c", b"3"))).sequence == 5
        assert db.del_(b"zz") is None
        assert db.del_(b"a").sequence == 6
        with pytest.raises(ValueError):
            db.del_range(b"x", b"a")
    ref.dump(tmp_path)
    lines = [line for line in capsys.readouterr().out.splitlines() if not line.endswith(".data")]
    # The trace in the module docstring, plus the delete.
    assert lines == [
        "       0  seq 1  PUT         b'a' b'1'",
        "      21  seq 2  BULK_BEGIN",
        "      40  seq 3  PUT         b'b' b'2'",
        "      61  seq 4  PUT         b'c' b'3'",
        "      82  seq 5  BULK_END",
        "     101  seq 6  DELETE      b'a'",
    ]


def test_reads_and_scans(tmp_path):
    with ref.DB.open(tmp_path) as db:
        for k in [b"a", b"b1", b"b2", b"c"]:
            db.put(k, k.upper(), NO_SYNC)
        assert db.get(b"b1") == b"B1" and db.get(b"zz") is None
        assert db.contains_key(b"c") and not db.contains_key(b"zz")
        assert list(db.keys_from(b"b")) == [b"b1", b"b2", b"c"]
        assert list(db.iter_from(b"b2")) == [(b"b2", b"B2"), (b"c", b"C")]
        assert list(db.rkeys_from(b"b2")) == [b"b2", b"b1", b"a"]
        assert list(db.rkeys_from()) == [b"c", b"b2", b"b1", b"a"]
        assert list(db.riter_from(b"a")) == [(b"a", b"A")]


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


def test_guards_and_snapshots(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db.put(b"price", b"10")
        db.put(b"r1", b"x")

        snap = db.snapshot()
        db.put(b"price", b"20")
        assert snap.get(b"price") == b"10" and db.get(b"price") == b"20"
        plan = ref.WritePlan(snap)
        plan.ensure_unchanged(b"price")
        plan.put(b"order", b"1")
        assert db.apply_batch(plan) is None
        with pytest.raises(RuntimeError, match="consumed"):
            snap.get(b"price")
        with pytest.raises(RuntimeError, match="applied"):
            plan.put(b"again", b"1")

        snap = db.snapshot()
        db.put(b"price", b"30")
        assert db.apply_batch(batch((b"price", b"40"), snapshot=snap)) is None  # written key moved

        snap = db.snapshot()
        db.put(b"new", b"1")
        db.del_(b"new")  # created and erased since: not a change
        plan = ref.WritePlan(snap)
        plan.ensure_range_unchanged(b"n", b"o")
        assert db.apply_batch(plan) is not None

        snap = db.snapshot()
        db.del_(b"r1")
        plan = ref.WritePlan(snap)
        plan.ensure_range_unchanged(b"r", b"s")
        assert db.apply_batch(plan) is None

        plan = ref.WritePlan()
        plan.ensure_absent(b"price")
        assert db.apply_batch(plan) is None
        with pytest.raises(ValueError):
            ref.WritePlan().ensure_unchanged(b"k")
        plan = ref.WritePlan()
        plan.ensure_present(b"k")
        with pytest.raises(ValueError, match="contradictory"):
            plan.ensure_absent(b"k")


# ---------------------------------------------------------------------------
# Recovery and I/O errors
# ---------------------------------------------------------------------------


def test_torn_batch_in_newest_file_is_cut(tmp_path):
    with ref.DB.open(tmp_path) as db:
        db.put(b"a", b"1")
        db.apply_batch(batch((b"b", b"2"), (b"c", b"3")))
    path = file_starting_at(tmp_path, 1)
    with open(path, "r+b") as f:
        f.truncate(path.stat().st_size - 5)  # BULK_END torn
    with ref.DB.open(tmp_path) as db:
        assert list(db.keys_from()) == [b"a"]
    assert path.stat().st_size == len(ref.Entry(1, T.PUT, b"a", b"1").encode())


@pytest.mark.parametrize("torn_to", [10, 20])
def test_a_torn_first_write_of_a_file_is_cut(tmp_path, torn_to):
    """A file torn inside its first header has no first sequence. Nothing in
    it survives, so it goes whole, as in the engine."""
    with ref.DB.open(tmp_path) as db:
        db.put(b"a", b"1")
    with ref.DB.open(tmp_path) as db:
        db.put(b"b", b"2")
    second = file_starting_at(tmp_path, 2)
    with open(second, "r+b") as f:
        f.truncate(torn_to)
    with ref.DB.open(tmp_path) as db:
        assert list(db.keys_from()) == [b"a"]
    assert second.stat().st_size == 0


def test_a_file_whose_first_page_was_lost_is_cut(tmp_path):
    """A power cut can lose the first page of a file that was never synced
    and keep a later one. The file has no first sequence, and goes whole."""
    with ref.DB.open(tmp_path) as db:
        db.put(b"a", b"1")
    with ref.DB.open(tmp_path) as db:
        for i in range(5):
            db.put(f"k{i}".encode(), b"v", NO_SYNC)
    second = file_starting_at(tmp_path, 2)
    raw = bytearray(second.read_bytes())
    raw[:8] = bytes(8)
    second.write_bytes(bytes(raw))
    with ref.DB.open(tmp_path) as db:
        assert list(db.keys_from()) == [b"a"]
    assert second.stat().st_size == 0


def test_two_files_with_data_past_their_end_are_refused(tmp_path):
    """A crash tears only the file being written. An older file with no first
    sequence beside a torn newest one is damage, and neither is cut."""
    with ref.DB.open(tmp_path) as db:
        for i in range(5):
            db.put(f"k{i}".encode(), b"v", NO_SYNC)
    with ref.DB.open(tmp_path) as db:
        db.put(b"y", b"1")
        db.put(b"z", b"2")
    older, newest = file_starting_at(tmp_path, 1), file_starting_at(tmp_path, 6)
    raw = bytearray(older.read_bytes())
    raw[:8] = bytes(8)
    older.write_bytes(bytes(raw))
    with open(newest, "r+b") as f:
        f.truncate(newest.stat().st_size - 5)
    sizes = older.stat().st_size, newest.stat().st_size
    with pytest.raises(RuntimeError, match="corrupt data files"):
        ref.DB.open(tmp_path)
    assert (older.stat().st_size, newest.stat().st_size) == sizes


def test_damage_in_an_older_file_is_refused(tmp_path):
    opts = ref.Options(max_file_bytes=1)  # a file per write
    with ref.DB.open(tmp_path, opts) as db:
        db.put(b"a", b"1")
        db.put(b"b", b"2")
    oldest = file_starting_at(tmp_path, 1)
    raw = bytearray(oldest.read_bytes())
    raw[-1] ^= 0xFF
    oldest.write_bytes(bytes(raw))
    with pytest.raises(RuntimeError, match="corrupt data file"):
        ref.DB.open(tmp_path, opts)


def test_a_failed_sync_stops_writes(tmp_path, monkeypatch):
    db = ref.DB.open(tmp_path)
    db.put(b"a", b"1")
    real = ref._datasync

    def fail_once(fd):
        monkeypatch.setattr(ref, "_datasync", real)
        raise OSError(errno.EIO, "injected")

    monkeypatch.setattr(ref, "_datasync", fail_once)
    with pytest.raises(OSError):
        db.put(b"b", b"2")
    # A sync that succeeds now may not write what the failed one left behind.
    with pytest.raises(ref.DbDegraded):
        db.put(b"c", b"3", NO_SYNC)
    assert db.is_degraded and "injected" in db.degraded_reason
    assert db.get(b"a") == b"1"  # reads go on
    with pytest.raises(ref.DbDegraded):
        db.close()
    with ref.DB.open(tmp_path) as db:  # the failed write may be there or not, never in part
        assert db.get(b"a") == b"1" and db.get(b"b") in (None, b"2")
        assert not db.contains_key(b"c")
        db.put(b"c", b"3")


def test_a_torn_write_is_cut_at_reopen(tmp_path, monkeypatch):
    opts = ref.Options(max_file_bytes=300)
    db = ref.DB.open(tmp_path, opts)
    db.put(b"a", b"1")

    def write_half(fd, data, offset):
        os.pwrite(fd, data[: len(data) // 2], offset)
        raise OSError(errno.ENOSPC, "injected")

    with monkeypatch.context() as m:
        m.setattr(ref, "_pwrite_all", write_half)
        with pytest.raises(OSError):
            db.apply_batch(batch((b"b", b"2" * 50), (b"c", b"3" * 50)))
    with pytest.raises(ref.DbDegraded):
        db.close()
    # Reopened, the torn bytes are gone: later writes rotate the file away,
    # and a tail left in it would make the next open refuse it as damage.
    with ref.DB.open(tmp_path, opts) as db:
        assert list(db.keys_from()) == [b"a"]
        for i in range(20):
            db.put(f"k{i}".encode(), b"x" * 40)
    with ref.DB.open(tmp_path, opts) as db:
        assert len(list(db.keys_from())) == 21


def test_open_makes_the_newest_file_durable(tmp_path, monkeypatch):
    opts = ref.Options(max_file_bytes=1)  # a file per write
    with ref.DB.open(tmp_path, opts) as db:
        db.put(b"a", b"1")
        db.put(b"b", b"2")
    newest = file_starting_at(tmp_path, 2)
    synced = []
    real = ref._fullsync

    def record(fd):
        synced.append(os.fstat(fd).st_ino)
        real(fd)

    monkeypatch.setattr(ref, "_fullsync", record)
    ref.DB.open(tmp_path, opts).close()
    assert newest.stat().st_ino in synced


# ---------------------------------------------------------------------------
# Against the native engine, at the engine's interface
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
    start = rng.choice([*keys, b""])
    for scan in ("iter_from", "keys_from", "riter_from", "rkeys_from"):
        assert list(getattr(r, scan)(start)) == list(getattr(n, scan)(start)), (where, scan, start)


@pytest.mark.parametrize("seed", range(6))
def test_differential_against_native(tmp_path, bc, seed):
    """One seeded workload on both engines: every commit, conflict, sequence
    and read must agree, through rotation, reopen, snapshots held across other
    writes, and native vacuum (which the reference does not have and must not
    need)."""
    rng = random.Random(seed)
    keys = sorted({bytes(rng.choice(b"abc") for _ in range(rng.randint(1, 4)))
                   for _ in range(60)})
    max_file_bytes = 512

    def open_both():
        return (ref.DB.open(tmp_path / "ref", ref.Options(max_file_bytes=max_file_bytes)),
                bc.DB.open(str(tmp_path / "native"), native_options(bc, max_file_bytes)))

    r, n = open_both()
    snaps = []  # (reference, native); a plan consumes its pair, as in the engine

    def value():
        return bytes(rng.randrange(256) for _ in range(rng.randint(0, 40)))

    def key_range():
        a, b = rng.sample(keys, 2)  # distinct: from >= to is refused
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
                    rp.put(k, v)
                    np_.put(k, v)
                elif kind < 0.9:
                    k = rng.choice(keys)
                    rp.del_(k)
                    np_.del_(k)
                else:
                    a, b = key_range()
                    rp.del_range(a, b)
                    np_.del_range(a, b)
            for k in rng.sample(keys, rng.randint(0, 2)):
                guard = rng.choice(["present", "absent", *(["unchanged"] * 2 if pair else [])])
                getattr(rp, f"ensure_{guard}")(k)
                getattr(np_, f"ensure_{guard}")(k)
            if pair and rng.random() < 0.3:
                a, b = key_range()
                rp.ensure_range_unchanged(a, b)
                np_.ensure_range_unchanged(a, b)
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
            r.close()
            n.close()
            r, n = open_both()
    assert list(r.iter_from()) == list(n.iter_from())
    r.close()
    n.close()


# ---------------------------------------------------------------------------
# Replication, differentially: the same cluster script on both engines
# ---------------------------------------------------------------------------

MASKED_ID = b"<id>"  # a marker's id is random per node, so histories compare without it


class Replication:
    """One engine's replication interface behind the names the script uses.

    Histories are (sequence, type value, key, value) with marker ids masked;
    outcomes are "ok", "gap" (ValueError) or "fork" (DbChangeMarkerMismatch).
    """

    def __init__(self, root):
        self.root = Path(root)

    def outcome(self, source, follower, from_sequence=None):
        if from_sequence is None:
            from_sequence = follower.durable_sequence()
        header, entries = self.slice(source, from_sequence)
        try:
            follower.ingest(header, entries)
        except ValueError:
            return "gap"
        except self.fork_error:
            return "fork"
        return "ok"

    def catch_up(self, source, follower, max_bytes=None):
        slices = []
        while follower.durable_sequence() < source.durable_sequence():
            header, entries = self.slice(source, follower.durable_sequence(), max_bytes)
            follower.ingest(header, entries)
            slices.append([e[0] for e in self.normalize(entries)])
        return slices

    def history(self, db):
        _, entries = self.slice(db, 0)
        return self.normalize(entries)

    def bootstrap(self, source, name):
        files, through = self.manifest_files(source)
        path = self.root / name
        shutil.rmtree(path, ignore_errors=True)
        path.mkdir()
        for file in files:
            if Path(file).exists():
                shutil.copy(file, path / Path(file).name)
        follower = self.open(name, follower=True)
        assert follower.durable_sequence() == through
        return follower

    @staticmethod
    def mask(sequence, kind, key, value):
        return (sequence, kind, key, MASKED_ID if kind == int(T.CHANGE_MARKER) else value)


class ReferenceReplication(Replication):
    fork_error = ref.DbChangeMarkerMismatch

    def open(self, name, follower=False, max_file_bytes=1024):
        mode = ref.Mode.Follower if follower else ref.Mode.Leader
        return ref.DB.open(self.root / name, ref.Options(initial_mode=mode, max_file_bytes=max_file_bytes))

    def set_leader(self, db):
        db.set_mode(ref.Mode.Leader)

    def set_follower(self, db):
        db.set_mode(ref.Mode.Follower)

    def slice(self, source, from_sequence, max_bytes=None):
        batch = source.changes_since(source.snapshot(), from_sequence, max_bytes)
        return batch.header, list(batch.entries)

    def normalize(self, entries):
        return [self.mask(e.sequence, int(e.type), e.key, e.value) for e in entries]

    def manifest_files(self, source):
        manifest = source.create_manifest()
        return manifest.files, manifest.through_sequence

    def apply_batch(self, db, writes):
        return db.apply_batch(batch(*writes)).sequence


class NativeReplication(Replication):
    def __init__(self, bc, root):
        super().__init__(root)
        self.bc = bc
        self.fork_error = bc.DbChangeMarkerMismatch

    def open(self, name, follower=False, max_file_bytes=1024):
        opts = native_options(self.bc, max_file_bytes)
        opts.initial_mode = self.bc.Mode.Follower if follower else self.bc.Mode.Leader
        return self.bc.DB.open(str(self.root / name), opts)

    def set_leader(self, db):
        db.set_mode(self.bc.Mode.Leader)

    def set_follower(self, db):
        db.set_mode(self.bc.Mode.Follower)

    def slice(self, source, from_sequence, max_bytes=None):
        batch = source.changes_since(source.snapshot(), from_sequence, max_bytes)
        return batch.header, list(batch.entries)

    def normalize(self, entries):
        return [self.mask(e.sequence, e.entry_type.value, e.key, e.value) for e in entries]

    def manifest_files(self, source):
        manifest = source.create_manifest()
        files = [p for f in manifest.files for p in (f.data_path, f.hint_path)]
        return files, manifest.through_sequence

    def apply_batch(self, db, writes):
        plan = self.bc.WritePlan()
        for key, value in writes:
            plan.put(key, value)
        return db.apply_batch(plan).sequence


def replication_script(x):
    """The design's unplanned failover, then a gap, re-deliveries, a planned
    transfer and slicing, on one engine. Yields (step, observation)."""
    L = x.open("L")
    for i in range(10):
        yield f"put {i}", L.put(f"k{i:02}".encode(), b"L").sequence
    N = x.bootstrap(L, "N")
    G = x.bootstrap(L, "G")
    for i in range(10, 20):
        L.put(f"k{i:02}".encode(), b"L")
    yield "batch on L", x.apply_batch(L, [(b"b1", b"1"), (b"b2", b"2")])
    F = x.bootstrap(L, "F")
    yield "positions", (N.durable_sequence(), G.durable_sequence(), F.durable_sequence())
    header = x.slice(L, 0)[0]
    yield "origin header", (header.marker.since_sequence, header.marker.id, header.from_sequence)
    L.close()  # L dies

    x.set_leader(N)  # the wrong choice: F is ahead
    yield "N's first write after the marker", N.put(b"n", b"1").sequence
    yield "N history", x.history(N)
    yield "N's marker at 20", x.slice(N, 20)[0].marker.since_sequence
    yield "F's marker at 20", x.slice(F, 20)[0].marker.since_sequence
    yield "N -> F", x.outcome(N, F)
    yield "N -> F from 5", x.outcome(N, F, from_sequence=5)
    yield "F untouched", (F.durable_sequence(), x.history(F))
    yield "N -> G catch up", x.catch_up(N, G)
    yield "G history", x.history(G)
    L = x.open("L", follower=True)
    yield "L rejoins", (L.durable_sequence(), x.outcome(N, L))
    yield "N -> F, empty slice at F's position", x.outcome(N, F, from_sequence=F.durable_sequence())

    # A gap: a fresh follower asked to start past its position.
    E = x.open("E", follower=True)
    yield "gap", x.outcome(N, E, from_sequence=3)
    yield "E untouched", (E.durable_sequence(), x.history(E))
    yield "E from 0, one unit per slice", x.catch_up(N, E, max_bytes=1)

    # Re-delivery of the same history, with the marker in the range.
    yield "N -> G from 5 again", x.outcome(N, G, from_sequence=5)
    yield "N -> G from the marker", x.outcome(N, G, from_sequence=11)
    yield "N -> G from 0", x.outcome(N, G, from_sequence=0)

    # Planned transfer: N steps down, G is promoted, N follows G.
    x.set_follower(N)
    x.set_leader(G)
    yield "G's first write after its marker", G.put(b"g", b"1").sequence
    yield "G -> N", x.catch_up(G, N)
    yield "G -> E", x.catch_up(G, E, max_bytes=1)
    yield "N -> F after the transfer", x.outcome(G, F)

    # Re-bootstrap F and L from G: everything converges.
    F.close()
    L.close()
    F = x.bootstrap(G, "F2")
    L = x.bootstrap(G, "L2")
    yield "batch on G", x.apply_batch(G, [(b"b1", b"3"), (b"b3", b"4")])
    for name, node in (("N", N), ("E", E), ("F", F), ("L", L)):
        x.catch_up(G, node)
        yield f"{name} converged", (node.durable_sequence(), x.history(node) == x.history(G),
                                    list(node.iter_from()) == list(G.iter_from()))
    yield "G history", x.history(G)
    yield "G keys", list(G.iter_from())
    for node in (N, G, E, F, L):
        node.close()
    N = x.open("N")  # reopening in Leader mode is not a promotion
    yield "N reopened", (N.durable_sequence(), N.put(b"z", b"1").sequence, x.history(N)[-3:])
    N.close()


def test_replication_differential_against_native(tmp_path, bc):
    """The same scripted cluster on the reference and on the engine: every
    outcome (accepted, gap, fork), durable sequence and history must agree,
    with marker ids, random per node, masked."""
    reference = replication_script(ReferenceReplication(tmp_path / "ref"))
    native = replication_script(NativeReplication(bc, tmp_path / "native"))
    steps = 0
    for (r_step, r_seen), (n_step, n_seen) in zip(reference, native, strict=True):
        assert r_step == n_step
        assert r_seen == n_seen, r_step
        steps += 1
    assert steps > 30


# ---------------------------------------------------------------------------
# The Pythonic wrapper, ext.py, on both backends
# ---------------------------------------------------------------------------


def ext_script(ext, db):
    """Exercises the wrapper's own logic: the dict protocol, prefix scans,
    batches, and transactions with read-your-own-writes and conflicts."""
    out = []
    db[b"user:1"] = b"alice"
    db[b"user:2"] = b"bob"
    db[b"zz"] = b"last"
    del db[b"user:2"]
    del db[b"absent"]
    out += [db[b"user:1"], b"user:2" in db, db.get(b"nope", b"dflt")]
    with db.batch(sync=False) as b:
        b[b"user:3"] = b"carol"
        b[b"user:4"] = b"dave"
        b.delete_range(b"user:4", b"user:5")
    out.append(b.result.sequence)
    with db.transaction() as txn:
        txn[b"user:5"] = txn[b"user:1"]
        txn.delete_range(b"user:1", b"user:2")
        out += [txn.get(b"user:1"), txn[b"user:5"], b"user:3" in txn]
    out.append(txn.result.sequence)
    try:
        with db.transaction() as txn:
            txn.ensure_unchanged(b"user:3")
            db[b"user:3"] = b"changed"
            txn[b"user:6"] = b"x"
    except ext.ConflictError:
        out.append("conflict")
    out += [list(db.items()), list(db.ritems(b"user:4")), list(db.prefix(b"user:")),
            list(db.rprefix(b"user:")), list(db.keys(b"user:3")), list(db.rkeys())]
    with db.snapshot() as snap:
        db[b"user:7"] = b"late"
        out += [list(snap.prefix(b"user:")), b"user:7" in snap]
    out.append(db.delete_range(b"user:", b"user;").sequence)
    out.append(list(db.items()))
    return out


def test_ext_runs_the_same_on_both_backends(tmp_path):
    ext = pytest.importorskip("bytecaskdb")
    with ext.DB.open(str(tmp_path / "native")) as db:
        native = ext_script(ext, db)
    with ext.DB.open(str(tmp_path / "ref"), backend=ref) as db:
        reference = ext_script(ext, db)
    assert reference == native
    assert "conflict" in native


# ---------------------------------------------------------------------------
# The file format, in both directions
# ---------------------------------------------------------------------------


def fill(put, rng):
    for _ in range(300):
        put(f"k{rng.randrange(100):03}".encode(),
            bytes(rng.randrange(256) for _ in range(rng.randint(0, 30))))


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
