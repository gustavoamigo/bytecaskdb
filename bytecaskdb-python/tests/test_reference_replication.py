# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# Replication in the Python reference implementation (reference/bytecask_ref.py):
# changes_since and ingest, promotion with change markers, and the fork and gap
# checks of #397. The cluster model at the end runs a leader and followers
# through writes, lag, transfers, promotions of the wrong follower, crashes,
# zombie leaders and re-delivery, and checks every ingest against an oracle
# that compares the two histories directly.

import errno
import random
import shutil
import sys
from collections import Counter
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
import bytecask_ref as ref  # noqa: E402
from bytecask_ref import ChangeHeader, Entry, Mode  # noqa: E402
from bytecask_ref import EntryType as T  # noqa: E402

NO_SYNC = ref.WriteOptions(sync=False)
ORIGIN = ref.ORIGIN_MARKER


def open_follower(path, **opts):
    return ref.DB.open(path, ref.Options(initial_mode=Mode.Follower, **opts))


def slice_from(source, from_sequence, max_bytes=None):
    batch = source.changes_since(source.snapshot(), from_sequence, max_bytes)
    return batch.header, list(batch.entries)


def replicate(source, follower, from_sequence=None, max_bytes=None):
    """One changes_since -> ingest, from the follower's position unless given."""
    if from_sequence is None:
        from_sequence = follower.durable_sequence()
    header, entries = slice_from(source, from_sequence, max_bytes)
    follower.ingest(header, entries)
    return entries


def catch_up(source, follower, max_bytes=None):
    while follower.durable_sequence() < source.durable_sequence():
        replicate(source, follower, max_bytes=max_bytes)


def bootstrap(source, path):
    """A new follower from the source's manifest."""
    manifest = source.create_manifest()
    path = Path(path)
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir()
    for file in manifest.files:
        shutil.copy(file, path / file.name)
    follower = open_follower(path)
    assert follower.durable_sequence() == manifest.through_sequence
    return follower


def history(db):
    """Every durable entry, in sequence order."""
    return list(db.changes_since(db.snapshot(), 0).entries)


def markers(db):
    return [ref._marker_of(e) for e in history(db) if e.type is T.CHANGE_MARKER]


def marker_at(db, sequence):
    return db.changes_since(db.snapshot(), sequence).header.marker


def batch(*writes):
    plan = ref.WritePlan()
    for key, value in writes:
        plan.put(key, value)
    return plan


# ---------------------------------------------------------------------------
# changes_since and ingest
# ---------------------------------------------------------------------------


def test_round_trip_in_slices_keeps_batches_whole(tmp_path):
    leader = ref.DB.open(tmp_path / "leader", ref.Options(max_file_bytes=300))
    follower = open_follower(tmp_path / "follower", max_file_bytes=200)
    for i in range(30):
        leader.put(f"k{i:02}".encode(), b"v" * (i % 7), NO_SYNC)
        if i % 5 == 0:
            leader.apply_batch(batch((b"b1", bytes([i])), (b"b2", b"x"), (b"b3", b"y")), NO_SYNC)
    leader.del_(b"k03", NO_SYNC)
    leader.del_range(b"k10", b"k15", NO_SYNC)
    last = leader.put(b"last", b"!")
    assert leader.durable_sequence() == last.sequence  # its sync covers every write before it

    slices = []
    while follower.durable_sequence() < leader.durable_sequence():
        slices.append(replicate(leader, follower, max_bytes=120))
    assert len(slices) > 5
    for entries in slices:
        assert entries, "behind the leader, a slice is never empty"
        sequences = [e.sequence for e in entries]
        assert sequences == sorted(set(sequences))
        begins = sum(e.type is T.BULK_BEGIN for e in entries)
        assert begins == sum(e.type is T.BULK_END for e in entries), "cut inside a batch"
        assert entries[-1].type is not T.BULK_BEGIN
    assert any(e.type is T.RANGE_DELETE for s in slices for e in s)

    assert list(follower.iter_from()) == list(leader.iter_from())
    assert history(follower) == history(leader)
    assert replicate(leader, follower) == []  # caught up: an empty slice is accepted
    follower.close()
    with open_follower(tmp_path / "follower", max_file_bytes=200) as follower:
        assert list(follower.iter_from()) == list(leader.iter_from())
        assert follower.durable_sequence() == leader.durable_sequence()
    leader.close()


def test_changes_since_ships_what_was_durable_and_stepping_down_syncs(tmp_path):
    with ref.DB.open(tmp_path / "leader") as leader:
        leader.put(b"a", b"1")
        assert leader.durable_sequence() == 1
        leader.put(b"b", b"2", NO_SYNC)
        assert leader.durable_sequence() == 1
        snap = leader.snapshot()
        assert snap.get(b"b") == b"2"  # visible
        assert [e.key for e in leader.changes_since(snap, 0).entries] == [b"a"]  # not shipped
        leader.put(b"c", b"3")  # its sync covers b
        assert leader.durable_sequence() == 3
        assert [e.key for e in history(leader)] == [b"a", b"b", b"c"]
        assert leader.changes_since(leader.snapshot(), 0).header == ChangeHeader(ORIGIN, 0)

        leader.put(b"d", b"4", NO_SYNC)
        leader.set_mode(Mode.Follower)  # stepping down makes d durable, so it can be shipped
        assert leader.durable_sequence() == 4 and leader.mode is Mode.Follower
        for write in (lambda: leader.put(b"e", b"5"), lambda: leader.del_(b"a"),
                      lambda: leader.del_range(b"a", b"b"), lambda: leader.apply_batch(ref.WritePlan())):
            with pytest.raises(ref.DbFollowerMode):
                write()
        assert markers(leader) == []  # stepping down writes nothing


def test_promotion_writes_a_marker_and_only_a_promotion_does(tmp_path):
    leader = ref.DB.open(tmp_path / "leader")
    leader.put(b"a", b"1")
    leader.set_mode(Mode.Leader)  # leader to leader: nothing written
    assert leader.durable_sequence() == 1 and markers(leader) == []

    follower = open_follower(tmp_path / "follower")
    replicate(leader, follower)
    leader.set_mode(Mode.Follower)
    follower.set_mode(Mode.Leader)
    assert follower.mode is Mode.Leader
    (marker,) = markers(follower)
    assert marker.since_sequence == 2 and marker.id != 0
    assert follower.durable_sequence() == 2, "the marker is synced before the mode changes"
    assert follower.put(b"b", b"2").sequence == 3, "the first write takes the sequence after the marker's"
    # The marker names the history from its sequence on; before it, the origin.
    assert marker_at(follower, 1) == ORIGIN
    assert marker_at(follower, 2) == marker and marker_at(follower, 99) == marker

    # The old leader follows the new one and receives the marker as an entry.
    catch_up(follower, leader)
    assert markers(leader) == [marker] and history(leader) == history(follower)
    assert marker_at(leader, 99) == marker

    # Reopening in Leader mode is not a promotion: nothing written, sequences go on.
    follower.close()
    with ref.DB.open(tmp_path / "follower") as reopened:
        assert markers(reopened) == [marker] and reopened.durable_sequence() == 3
        assert reopened.put(b"c", b"3").sequence == 4
        assert markers(reopened) == [marker]
    leader.close()


def test_markers_accumulate_across_promotions_and_survive_reopen(tmp_path):
    a = ref.DB.open(tmp_path / "a")
    a.put(b"k", b"1")
    b = bootstrap(a, tmp_path / "b")
    a.set_mode(Mode.Follower)
    b.set_mode(Mode.Leader)
    b.put(b"k", b"2")
    catch_up(b, a)
    b.set_mode(Mode.Follower)
    a.set_mode(Mode.Leader)
    a.put(b"k", b"3")
    catch_up(a, b)
    expected = markers(a)
    assert [m.since_sequence for m in expected] == [2, 4] and markers(b) == expected
    assert marker_at(a, 1) == ORIGIN and marker_at(a, 3) == expected[0] and marker_at(a, 4) == expected[1]
    durable = a.durable_sequence()
    assert b.durable_sequence() == durable
    for db, name in ((a, "a"), (b, "b")):
        db.close()
        with open_follower(tmp_path / name) as reopened:
            assert markers(reopened) == expected
            assert reopened.durable_sequence() == durable
            assert reopened.get(b"k") == b"3"


def test_a_follower_whose_writes_stopped_cannot_be_promoted(tmp_path, monkeypatch):
    leader = ref.DB.open(tmp_path / "leader")
    leader.put(b"a", b"1")
    follower = open_follower(tmp_path / "follower")
    real = ref._datasync

    def fail_once(fd):
        monkeypatch.setattr(ref, "_datasync", real)
        raise OSError(errno.EIO, "injected")

    monkeypatch.setattr(ref, "_datasync", fail_once)
    with pytest.raises(OSError):
        replicate(leader, follower)
    assert follower.is_degraded and follower.durable_sequence() == 0
    with pytest.raises(ref.DbDegraded):
        follower.set_mode(Mode.Leader)
    assert follower.mode is Mode.Follower
    with pytest.raises(ref.DbDegraded):
        follower.ingest(ChangeHeader(ORIGIN, 0), [])
    with pytest.raises(ref.DbDegraded):
        follower.close()
    with open_follower(tmp_path / "follower") as follower:  # the slice is there whole or not at all
        assert markers(follower) == [] and follower.durable_sequence() in (0, 1)
    leader.close()


def test_a_promotion_whose_sync_fails_leaves_the_mode(tmp_path, monkeypatch):
    follower = open_follower(tmp_path / "f")
    real = ref._datasync

    def fail_once(fd):
        monkeypatch.setattr(ref, "_datasync", real)
        raise OSError(errno.EIO, "injected")

    monkeypatch.setattr(ref, "_datasync", fail_once)
    with pytest.raises(OSError):
        follower.set_mode(Mode.Leader)
    assert follower.mode is Mode.Follower and follower.is_degraded
    with pytest.raises(ref.DbDegraded):
        follower.close()
    # The marker may or may not have reached the disk; a reopen in Leader mode adds none.
    with ref.DB.open(tmp_path / "f") as reopened:
        found = markers(reopened)
        assert len(found) <= 1
        reopened.put(b"a", b"1")
        assert markers(reopened) == found


# ---------------------------------------------------------------------------
# The checks: gap, fork, re-delivery
# ---------------------------------------------------------------------------


def test_a_gap_is_refused_before_anything_is_written(tmp_path):
    leader = ref.DB.open(tmp_path / "leader")
    for i in range(5):
        leader.put(bytes([97 + i]), b"v", NO_SYNC)
    leader.put(b"z", b"v")  # 6 entries, all durable
    follower = open_follower(tmp_path / "follower")
    replicate(leader, follower, max_bytes=1)  # one unit per slice
    replicate(leader, follower, max_bytes=1)
    assert follower.durable_sequence() == 2

    header, entries = slice_from(leader, 3)  # starts past the follower: 3 is missing
    assert entries[0].sequence == 4
    with pytest.raises(ValueError, match="gap"):
        follower.ingest(header, entries)
    assert follower.durable_sequence() == 2 and len(history(follower)) == 2
    replicate(leader, follower)  # from 2: accepted
    assert history(follower) == history(leader)
    leader.close()
    follower.close()


def test_promoting_the_less_advanced_follower_forks_the_one_ahead(tmp_path):
    """The unplanned failover of the design: L dies at 20; N is at 10, F at 20."""
    L = ref.DB.open(tmp_path / "L")
    for i in range(10):
        L.put(f"k{i:02}".encode(), b"L")
    N = bootstrap(L, tmp_path / "N")
    G = bootstrap(L, tmp_path / "G")
    for i in range(10, 20):
        L.put(f"k{i:02}".encode(), b"L")
    F = bootstrap(L, tmp_path / "F")
    assert (N.durable_sequence(), G.durable_sequence(), F.durable_sequence()) == (10, 10, 20)
    L.close()  # L dies

    N.set_mode(Mode.Leader)  # the wrong choice: F is ahead of N
    N.put(b"n", b"1")
    (marker,) = markers(N)
    assert marker.since_sequence == 11
    assert marker_at(N, 20) == marker and marker_at(F, 20) == ORIGIN

    # F holds L's 11..20, which N never had: refused, and nothing is written.
    before = history(F)
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(N, F)
    # Re-delivered from an earlier point, the marker is in the slice: refused too.
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(N, F, from_sequence=5)
    assert history(F) == before and F.durable_sequence() == 20

    # G, at N's position, catches up and receives the marker.
    catch_up(N, G)
    assert markers(G) == [marker] and history(G) == history(N)

    # L comes back with writes nobody replicated: refused the same way.
    L = open_follower(tmp_path / "L")
    assert L.durable_sequence() == 20
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(N, L)
    # Re-bootstrapped from N, everything converges.
    F.close()
    L.close()
    F = bootstrap(N, tmp_path / "F2")
    L = bootstrap(N, tmp_path / "L2")
    N.put(b"n", b"2")
    for follower in (F, L, G):
        catch_up(N, follower)
        assert history(follower) == history(N)
        assert list(follower.iter_from()) == list(N.iter_from())
    for db in (N, F, L, G):
        db.close()


def test_a_zombie_leaders_writes_are_refused(tmp_path):
    """An old leader reopened in Leader mode in place keeps writing, without a
    marker. Once it steps down and follows the promoted node, the promoted
    node's marker gives it away."""
    L = ref.DB.open(tmp_path / "L")
    L.put(b"a", b"1")
    N = bootstrap(L, tmp_path / "N")
    L.close()
    N.set_mode(Mode.Leader)  # marker at 2
    L = ref.DB.open(tmp_path / "L")  # the forbidden reopen in place
    L.put(b"zombie", b"1")  # 2, no marker
    L.put(b"zombie", b"2")  # 3
    L.set_mode(Mode.Follower)
    assert markers(L) == [] and marker_at(L, 3) == ORIGIN
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(N, L)
    # Nothing was applied either way: N has nothing above 2, L stays as it was.
    assert L.durable_sequence() == 3 and L.get(b"zombie") == b"2"
    L.close()
    N.close()


def test_re_delivery_of_the_same_history_is_accepted(tmp_path):
    leader = ref.DB.open(tmp_path / "leader")
    for i in range(6):
        leader.put(bytes([97 + i]), b"1")
    follower = open_follower(tmp_path / "follower")
    catch_up(leader, follower)
    assert replicate(leader, follower, from_sequence=3)  # 4..6 again: all duplicates
    assert follower.durable_sequence() == 6 and len(history(follower)) == 6
    leader.put(b"g", b"1")
    leader.put(b"h", b"1")
    replicate(leader, follower, from_sequence=3)  # 4..8: the duplicates are skipped, 7 and 8 applied
    assert history(follower) == history(leader)

    # With a promotion in the re-delivered range.
    leader.set_mode(Mode.Follower)
    follower.set_mode(Mode.Leader)  # marker at 9
    follower.put(b"i", b"1")  # 10
    catch_up(follower, leader)
    assert markers(leader) == markers(follower)
    replicate(follower, leader, from_sequence=4)  # 5..10 again, marker included: same history
    replicate(follower, leader, from_sequence=9)  # from the marker itself
    assert history(leader) == history(follower)
    leader.close()
    follower.close()


def test_a_slice_cut_before_the_followers_marker_is_accepted(tmp_path):
    """The comparison point stops at the slice's last entry. B holds the marker
    at 6; a re-delivered slice from 2 cut at 4 says nothing about 6 and must
    pass; compared over (from, P] it would be refused."""
    A = ref.DB.open(tmp_path / "A")
    for i in range(5):
        A.put(bytes([97 + i]), b"1")
    G = bootstrap(A, tmp_path / "G")
    B = bootstrap(A, tmp_path / "B")
    A.set_mode(Mode.Follower)
    G.set_mode(Mode.Leader)  # marker at 6
    G.put(b"g", b"1")  # 7
    G.put(b"h", b"1")  # 8
    catch_up(G, B, max_bytes=1)  # unit by unit: the marker alone in a slice
    (marker,) = markers(B)
    assert marker.since_sequence == 6 and B.durable_sequence() == 8

    header, entries = slice_from(G, 2, max_bytes=1)
    assert [e.sequence for e in entries] == [3]  # cut well before the marker
    assert header == ChangeHeader(ORIGIN, 2)
    B.ingest(header, entries)  # X = 3: origin on both sides
    header, entries = slice_from(G, 2, max_bytes=40)
    assert entries[-1].sequence < 6
    B.ingest(header, entries)
    header, entries = slice_from(G, 2)  # uncut: the marker is in the slice and matches
    assert any(e.type is T.CHANGE_MARKER for e in entries)
    B.ingest(header, entries)
    assert B.durable_sequence() == 8 and history(B) == history(G)

    # Cut or not, a slice from a forked source is refused once the comparison
    # point reaches the divergence. A at 5 is promoted too (split brain).
    A.set_mode(Mode.Leader)  # A's own marker at 6
    A.put(b"split", b"1")  # 7
    header, entries = slice_from(A, 2, max_bytes=1)  # [3]: nothing to see yet
    B.ingest(header, entries)
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(A, B, from_sequence=2)  # X = min(8, 7) = 7: A's marker at 6 is not B's
    with pytest.raises(ref.DbChangeMarkerMismatch):
        replicate(A, B)  # from 8: an empty slice, compared at 8
    assert B.durable_sequence() == 8 and history(B) == history(G)
    for db in (A, B, G):
        db.close()


def test_malformed_slices_and_wrong_modes_are_refused(tmp_path):
    leader = ref.DB.open(tmp_path / "leader")
    follower = open_follower(tmp_path / "follower")
    header = ChangeHeader(ORIGIN, 0)
    marker = Entry(2, T.CHANGE_MARKER, value=bytes(8))
    cases = {
        "not increasing": [Entry(2, T.PUT, b"a", b"1"), Entry(1, T.PUT, b"b", b"1")],
        "repeated": [Entry(1, T.PUT, b"a", b"1"), Entry(1, T.PUT, b"b", b"1")],
        "at from": [Entry(0, T.PUT, b"a", b"1")],
        "ends inside a batch": [Entry(1, T.BULK_BEGIN), Entry(2, T.PUT, b"a", b"1")],
        "end out of place": [Entry(1, T.BULK_END)],
        "begin inside a batch": [Entry(1, T.BULK_BEGIN), Entry(2, T.BULK_BEGIN), Entry(3, T.BULK_END)],
        "marker inside a batch": [Entry(1, T.BULK_BEGIN), marker, Entry(3, T.BULK_END)],
        "marker with a key": [Entry(1, T.CHANGE_MARKER, b"k", bytes(8))],
        "marker with a short id": [Entry(1, T.CHANGE_MARKER, value=b"short")],
        "key too long": [Entry(1, T.PUT, b"k" * 5000, b"1")],
    }
    for name, entries in cases.items():
        with pytest.raises(ValueError):
            follower.ingest(header, entries)
        assert history(follower) == [], name
    with pytest.raises(ValueError, match="follower"):
        leader.ingest(header, [])
    with pytest.raises(ref.DbFollowerMode):
        follower.put(b"k", b"v")
    follower.ingest(header, [Entry(1, T.BULK_BEGIN), Entry(2, T.PUT, b"a", b"1"), Entry(3, T.BULK_END),
                             Entry(4, T.CHANGE_MARKER, value=bytes(8))])
    assert follower.durable_sequence() == 4 and follower.get(b"a") == b"1"
    leader.close()
    follower.close()


def test_on_disk_a_marker_inside_a_batch_and_an_unknown_type_are_damage():
    good = [Entry(1, T.PUT, b"a", b"1"), Entry(2, T.CHANGE_MARKER, value=bytes(8))]
    bad_batch = [Entry(3, T.BULK_BEGIN), Entry(4, T.CHANGE_MARKER, value=bytes(8)), Entry(5, T.BULK_END)]
    buf = b"".join(e.encode() for e in good + bad_batch)
    committed, end = ref.scan_committed(buf)
    assert [p.entry for p in committed] == good
    assert end == sum(e.size() for e in good)

    unknown = ref._HEADER.pack(3, 9, 0, 0)
    unknown += ref._CRC.pack(ref.crc32c(unknown))
    committed, end = ref.scan_committed(b"".join(e.encode() for e in good) + unknown)
    assert [p.entry for p in committed] == good and end == sum(e.size() for e in good)


# ---------------------------------------------------------------------------
# The cluster model
# ---------------------------------------------------------------------------


class Cluster:
    """A leader and followers as ref.DB instances, driven by random events.

    Every ingest is checked against an oracle on the two histories: with
    P the follower's durable sequence and X = min(P, the slice's last
    sequence), the slice must be refused as a gap when it starts past P, as
    a fork when the histories up to X are not one a prefix of the other, and
    accepted otherwise. Zombies (an old leader reopened in Leader mode in
    place) and promotions of the least advanced follower make the forks.
    """

    def __init__(self, root, rng, names, max_file_bytes):
        self.root, self.rng, self.names = root, rng, names
        self.opts = {"max_file_bytes": max_file_bytes}
        self.db = {}  # name -> DB, or None when down
        self.gen = Counter()  # bootstraps per name: each gets a fresh directory
        self.forked = set()  # refused as a fork; needs a bootstrap
        self.stats = Counter()
        self.keys = [bytes([c]) for c in b"abcdefgh"]
        self.leader = names[0]
        self.db[self.leader] = ref.DB.open(self.path(self.leader), ref.Options(**self.opts))
        for name in names[1:]:
            self.bootstrap(name)

    def path(self, name):
        return self.root / f"{name}-{self.gen[name]}"

    def up(self):
        return [n for n, db in self.db.items() if db is not None]

    def followers(self):
        return [n for n in self.up() if n != self.leader]

    def bootstrap(self, name):
        if self.db.get(name) is not None:
            self.db[name].close()
        self.gen[name] += 1
        self.db[name] = bootstrap(self.db[self.leader], self.path(name))
        self.forked.discard(name)
        self.stats["bootstraps"] += 1

    # ── events ──

    def write(self):
        db, rng = self.db[self.leader], self.rng
        opts = ref.WriteOptions(sync=rng.random() < 0.3)
        dice = rng.random()
        if dice < 0.5:
            db.put(rng.choice(self.keys), bytes([rng.randrange(256)]), opts)
        elif dice < 0.7:
            db.del_(rng.choice(self.keys), opts)
        else:
            plan = ref.WritePlan()
            for _ in range(rng.randint(2, 4)):
                plan.put(rng.choice(self.keys), bytes([rng.randrange(256)]))
            if rng.random() < 0.3:
                a, b = sorted(rng.sample(self.keys, 2))
                plan.del_range(a, b)
            db.apply_batch(plan, opts)
        self.stats["writes"] += 1

    def replicate(self, name, from_sequence=None, max_bytes=None):
        """One oracle-checked slice from the leader; returns the outcome."""
        source, follower, rng = self.db[self.leader], self.db[name], self.rng
        P = follower.durable_sequence()
        if from_sequence is None:
            dice = rng.random()
            if dice < 0.1:
                from_sequence = max(0, P - rng.randint(1, 12))  # a re-delivery
            elif dice < 0.15:
                from_sequence = P + rng.randint(1, 5)  # a gap
            else:
                from_sequence = P
            max_bytes = rng.choice([None, None, 1, 30, 100, 400])
        header, entries = slice_from(source, from_sequence, max_bytes)
        expected = self.oracle(source, follower, from_sequence, entries)
        where = f"{name} from {from_sequence} ({len(entries)} entries, P={P})"
        try:
            follower.ingest(header, entries)
        except ref.DbChangeMarkerMismatch:
            assert expected == "fork", where
            self.forked.add(name)
        except ValueError as e:
            assert expected == "gap" and "gap" in str(e), where
        else:
            assert expected == "ok", where
            new_P = follower.durable_sequence()
            last = entries[-1].sequence if entries else from_sequence
            assert new_P == max(P, last)
            # The slice vouches for the histories up to its last entry, and
            # applied what lay above P; above a slice cut short of P the
            # follower may still differ, and its next request from P says so.
            upto = min(new_P, last)
            assert ([e for e in history(follower) if e.sequence <= upto]
                    == [e for e in history(source) if e.sequence <= upto]), where
        assert follower.durable_sequence() >= P
        self.stats[expected] += 1
        if from_sequence < P:
            self.stats["re-deliveries"] += 1
        return expected

    def sync(self, name):
        """Catches name up with the leader, slice by slice; "fork" if refused.

        At least one slice is delivered even when name is not behind: an empty
        slice still compares the two histories at name's position, which is
        how a node ahead of the leader on another history is found out.
        """
        while True:
            if self.replicate(name, self.db[name].durable_sequence()) == "fork":
                return "fork"
            if self.db[name].durable_sequence() >= self.db[self.leader].durable_sequence():
                return "ok"

    @staticmethod
    def oracle(source, follower, from_sequence, entries):
        P = follower.durable_sequence()
        if from_sequence > P:
            return "gap"
        X = min(P, entries[-1].sequence if entries else from_sequence)
        hs = [e for e in history(source) if e.sequence <= X]
        hf = [e for e in history(follower) if e.sequence <= X]
        short, long = sorted((hs, hf), key=len)
        return "ok" if long[:len(short)] == short else "fork"

    def planned_transfer(self):
        candidates = [n for n in self.followers() if n not in self.forked]
        if not candidates:
            return
        target = self.rng.choice(candidates)
        old = self.db[self.leader]
        old.set_mode(Mode.Follower)
        if self.sync(target) == "fork":
            # The target is not on the leader's history: the transfer is
            # called off, and the old leader is promoted again.
            old.set_mode(Mode.Leader)
            self.stats["transfers aborted"] += 1
            return
        assert self.db[target].durable_sequence() == old.durable_sequence()
        self.db[target].set_mode(Mode.Leader)
        self.leader = target
        self.stats["transfers"] += 1

    def unplanned_promotion(self, least):
        candidates = [n for n in self.followers() if n not in self.forked]
        if not candidates:
            return
        old = self.leader
        self.db[old].close()  # the leader is lost; what it did not ship is its own
        self.db[old] = None
        durables = {n: self.db[n].durable_sequence() for n in candidates}
        pick = (min if least else max)(candidates, key=durables.get)
        self.db[pick].set_mode(Mode.Leader)
        self.leader = pick
        self.stats["promotions least" if least else "promotions most"] += 1

    def rejoin(self, name):
        if self.rng.random() < 0.4:
            # A zombie: the old leader reopened in Leader mode in place takes
            # writes without a marker, then is told to follow.
            db = ref.DB.open(self.path(name), ref.Options(**self.opts))
            for _ in range(self.rng.randint(1, 3)):
                db.put(self.rng.choice(self.keys), b"zombie", ref.WriteOptions(sync=True))
            db.set_mode(Mode.Follower)
            self.stats["zombies"] += 1
        else:
            db = open_follower(self.path(name), **self.opts)
        self.db[name] = db

    def restart(self, name):
        durable = self.db[name].durable_sequence()
        self.db[name].close()
        self.db[name] = open_follower(self.path(name), **self.opts)
        assert self.db[name].durable_sequence() == durable

    def step(self):
        rng = self.rng
        dice = rng.random()
        followers = self.followers()
        down = [n for n, db in self.db.items() if db is None]
        if dice < 0.40:
            self.write()
        elif dice < 0.72 and followers:
            self.replicate(rng.choice(followers))
        elif dice < 0.77:
            self.planned_transfer()
        elif dice < 0.84:
            self.unplanned_promotion(least=rng.random() < 0.5)
        elif dice < 0.89 and followers:
            self.restart(rng.choice(followers))
        elif dice < 0.95 and down:
            self.rejoin(rng.choice(down))
        elif self.forked:
            self.bootstrap(rng.choice(sorted(self.forked)))

    def converge(self):
        """Brings every node onto the leader's history and checks they agree."""
        # A sync of nothing makes the leader's unsynced writes durable, so its
        # position no longer moves when a bootstrap below seals its file.
        self.db[self.leader].apply_batch(ref.WritePlan(), ref.WriteOptions(sync=True))
        for name in self.names:
            if self.db[name] is None:
                self.rejoin(name)
        for name in self.followers():
            if name in self.forked or self.sync(name) == "fork":
                self.bootstrap(name)
                assert self.sync(name) == "ok"
        leader = self.db[self.leader]
        for name in self.followers():
            assert history(self.db[name]) == history(leader), name
            assert list(self.db[name].iter_from()) == list(leader.iter_from()), name
            assert self.db[name].durable_sequence() == leader.durable_sequence()

    def close(self):
        for db in self.db.values():
            if db is not None:
                db.close()


@pytest.mark.parametrize("seed", range(8))
def test_cluster_model(tmp_path, seed):
    rng = random.Random(seed)
    cluster = Cluster(tmp_path, rng, ["n0", "n1", "n2", "n3"], max_file_bytes=rng.choice([150, 600, 4000]))
    for _ in range(350):
        cluster.step()
    cluster.converge()
    cluster.close()
    s = cluster.stats
    assert s["ok"] > 40 and s["gap"] >= 1 and s["re-deliveries"] >= 1, dict(s)
    assert s["promotions least"] + s["zombies"] >= 1, dict(s)
    assert s["fork"] >= 1, dict(s)
