import shutil
import struct
from pathlib import Path

import pytest
import bytecaskdb._bytecaskdb as bc

T = bc.EntryType
ORIGIN = bc.ORIGIN_MARKER
MARKER_ID = struct.Struct("<Q")  # a ChangeMarker entry's value


@pytest.fixture
def db(tmp_path):
    return bc.DB.open(str(tmp_path / "testdb"))


@pytest.fixture
def nosync():
    opts = bc.WriteOptions()
    opts.sync = False
    return opts


def open_follower(path, **opts):
    options = bc.Options()
    options.initial_mode = bc.Mode.Follower
    for name, value in opts.items():
        setattr(options, name, value)
    return bc.DB.open(str(path), options)


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
    """A new follower from the source's manifest: its data and hint files copied."""
    manifest = source.create_manifest()
    path = Path(path)
    shutil.rmtree(path, ignore_errors=True)
    path.mkdir()
    for file in manifest.files:
        for name in (file.data_path, file.hint_path):
            if Path(name).exists():
                shutil.copy(name, path / Path(name).name)
    follower = open_follower(path)
    assert follower.durable_sequence() == manifest.through_sequence
    return follower


def history(db):
    """Every durable entry, in sequence order, as (sequence, type, key, value)."""
    return [(e.sequence, e.entry_type, e.key, e.value)
            for e in db.changes_since(db.snapshot(), 0).entries]


def markers(db):
    return [bc.ChangeMarker(seq, MARKER_ID.unpack(value)[0])
            for seq, kind, _, value in history(db) if kind == T.ChangeMarker]


def marker_at(db, sequence):
    return db.changes_since(db.snapshot(), sequence).header.marker


def test_mode_default_leader(db):
    assert db.mode == bc.Mode.Leader


def test_set_mode_follower_blocks_writes(db, nosync):
    db.set_mode(bc.Mode.Follower)
    assert db.mode == bc.Mode.Follower
    with pytest.raises(bc.DbFollowerMode):
        db.put(b"k", b"v", nosync)


def test_set_mode_back_to_leader(db, nosync):
    db.set_mode(bc.Mode.Follower)
    db.set_mode(bc.Mode.Leader)
    assert db.mode == bc.Mode.Leader
    db.put(b"k", b"v", nosync)
    assert db.get(b"k") == b"v"


def test_durable_sequence_empty(db):
    assert db.durable_sequence() == 0


def test_durable_sequence_increments(db):
    opts = bc.WriteOptions()
    opts.sync = True
    db.put(b"a", b"1", opts)
    seq1 = db.durable_sequence()
    assert seq1 > 0
    db.put(b"b", b"2", opts)
    seq2 = db.durable_sequence()
    assert seq2 > seq1


def test_create_manifest(db, nosync):
    db.put(b"k1", b"v1", nosync)
    db.put(b"k2", b"v2", nosync)
    manifest = db.create_manifest()
    assert manifest.through_sequence > 0
    assert len(manifest.files) > 0
    for fi in manifest.files:
        assert fi.file_id >= 0
        assert fi.data_path
        assert fi.hint_path
    # Snapshot in manifest is usable.
    assert manifest.snapshot.get(b"k1") == b"v1"


def test_changes_since(db):
    opts = bc.WriteOptions()
    opts.sync = True
    db.put(b"a", b"1", opts)
    db.put(b"b", b"2", opts)
    db.put(b"c", b"3", opts)

    snap = db.snapshot()
    batch = db.changes_since(snap, 0)
    assert batch.header == bc.ChangeHeader(ORIGIN, 0)
    entries = list(batch.entries)
    assert len(entries) >= 3

    # Entries are in sequence order.
    sequences = [e.sequence for e in entries]
    assert sequences == sorted(sequences)

    # Contains our puts.
    put_entries = [e for e in entries if e.entry_type == T.Put]
    keys = {bytes(e.key) for e in put_entries}
    assert b"a" in keys
    assert b"b" in keys
    assert b"c" in keys

    # Iterating the batch iterates its entries, once.
    assert [e.sequence for e in db.changes_since(snap, 0)] == sequences
    assert list(batch.entries) == []


def test_changes_since_from_sequence(db):
    opts = bc.WriteOptions()
    opts.sync = True
    db.put(b"a", b"1", opts)
    seq_after_a = db.durable_sequence()
    db.put(b"b", b"2", opts)

    snap = db.snapshot()
    batch = db.changes_since(snap, seq_after_a)
    assert batch.header.from_sequence == seq_after_a
    assert batch.header.marker == ORIGIN
    entries = list(batch.entries)
    put_entries = [e for e in entries if e.entry_type == T.Put]
    keys = {bytes(e.key) for e in put_entries}
    assert b"b" in keys
    assert b"a" not in keys


def test_ingest_basic(tmp_path):
    # Leader writes data.
    leader = bc.DB.open(str(tmp_path / "leader"))
    opts = bc.WriteOptions()
    opts.sync = True
    leader.put(b"k1", b"v1", opts)
    leader.put(b"k2", b"v2", opts)

    batch = leader.changes_since(leader.snapshot(), 0)

    # Follower ingests straight from the batch's lazy iterator.
    follower = open_follower(tmp_path / "follower")
    assert follower.mode == bc.Mode.Follower
    follower.ingest(batch.header, batch.entries)

    assert follower.get(b"k1") == b"v1"
    assert follower.get(b"k2") == b"v2"
    assert follower.durable_sequence() == leader.durable_sequence()


def test_ingest_idempotency(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    opts = bc.WriteOptions()
    opts.sync = True
    leader.put(b"k1", b"v1", opts)

    header, entries = slice_from(leader, 0)

    follower = open_follower(tmp_path / "follower")
    follower.ingest(header, entries)
    # Re-ingest same entries — should be idempotent.
    follower.ingest(header, entries)
    assert follower.get(b"k1") == b"v1"
    assert follower.durable_sequence() == 1


def test_ingest_from_reconstructed_data_entries(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    opts = bc.WriteOptions()
    opts.sync = True
    leader.put(b"k1", b"v1", opts)
    leader.put(b"k2", b"v2", opts)
    leader.del_(b"k1", opts)

    header, entries = slice_from(leader, 0)

    reconstructed = [
        bc.DataEntry(
            sequence=e.sequence,
            entry_type=e.entry_type,
            key=bytearray(e.key),
            value=bytearray(e.value),
        )
        for e in entries
    ]
    # The header reconstructs from its three integers too.
    wire = (header.marker.since_sequence, header.marker.id, header.from_sequence)
    rebuilt = bc.ChangeHeader(bc.ChangeMarker(wire[0], wire[1]), wire[2])
    assert rebuilt == header

    follower = open_follower(tmp_path / "follower")
    follower.ingest(rebuilt, reconstructed)

    assert follower.get(b"k1") is None
    assert follower.get(b"k2") == b"v2"


def test_leader_to_follower_replication(tmp_path):
    """End-to-end: leader writes, follower replicates via changes_since + ingest."""
    leader = bc.DB.open(str(tmp_path / "leader"))
    follower = open_follower(tmp_path / "follower")

    opts = bc.WriteOptions()
    opts.sync = True

    # Round 1: initial writes.
    leader.put(b"user:1", b"alice", opts)
    leader.put(b"user:2", b"bob", opts)
    replicate(leader, follower)

    assert follower.get(b"user:1") == b"alice"
    assert follower.get(b"user:2") == b"bob"

    # Round 2: incremental replication from the follower's position.
    leader.put(b"user:3", b"carol", opts)
    leader.del_(b"user:1", opts)
    replicate(leader, follower)

    assert follower.get(b"user:1") is None
    assert follower.get(b"user:2") == b"bob"
    assert follower.get(b"user:3") == b"carol"
    assert history(follower) == history(leader)


def test_entry_type_enum_values():
    assert T.Put is not None
    assert T.Delete is not None
    assert T.BulkBegin is not None
    assert T.BulkEnd is not None
    assert T.RangeDel is not None
    assert T.ChangeMarker is not None


def test_mode_enum_values():
    assert bc.Mode.Leader is not None
    assert bc.Mode.Follower is not None


# ---------------------------------------------------------------------------
# Change markers: promotion, gap, fork, re-delivery, slicing
# ---------------------------------------------------------------------------


def test_change_marker_and_header_are_values():
    assert ORIGIN == bc.ChangeMarker(0, 0) == bc.ChangeMarker()
    assert ORIGIN.since_sequence == 0 and ORIGIN.id == 0
    m = bc.ChangeMarker(since_sequence=7, id=2**64 - 1)
    assert m == bc.ChangeMarker(7, 2**64 - 1) and m != ORIGIN
    assert m != 7  # not a TypeError
    assert len({m, bc.ChangeMarker(7, 2**64 - 1), ORIGIN}) == 2
    assert repr(m) == f"ChangeMarker(since_sequence=7, id={2**64 - 1})"
    with pytest.raises(AttributeError):
        m.id = 1

    h = bc.ChangeHeader(m, 9)
    assert h.marker == m and h.from_sequence == 9
    assert h == bc.ChangeHeader(marker=m, from_sequence=9)
    assert h != bc.ChangeHeader(m, 8) and h != bc.ChangeHeader(ORIGIN, 9)
    assert bc.ChangeHeader() == bc.ChangeHeader(ORIGIN, 0)
    assert repr(h) == f"ChangeHeader(marker={m!r}, from_sequence=9)"


def test_change_marker_mismatch_is_a_bytecask_runtime_error():
    assert issubclass(bc.DbChangeMarkerMismatch, bc.ByteCaskError)
    assert issubclass(bc.DbChangeMarkerMismatch, RuntimeError)
    import bytecaskdb
    assert bytecaskdb.DbChangeMarkerMismatch is bc.DbChangeMarkerMismatch


def test_promotion_writes_a_marker_visible_in_changes_since(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    leader.put(b"a", b"1")
    leader.set_mode(bc.Mode.Leader)  # leader to leader: nothing written
    assert leader.durable_sequence() == 1 and markers(leader) == []

    follower = open_follower(tmp_path / "follower")
    replicate(leader, follower)
    assert marker_at(follower, 0) == ORIGIN and marker_at(follower, 1) == ORIGIN

    leader.set_mode(bc.Mode.Follower)
    follower.set_mode(bc.Mode.Leader)
    assert follower.mode == bc.Mode.Leader
    assert follower.durable_sequence() == 2, "the marker is synced before the mode changes"
    entries = history(follower)
    seq, kind, key, value = entries[-1]
    assert (seq, kind, key) == (2, T.ChangeMarker, b"")
    assert len(value) == 8
    (marker,) = markers(follower)
    assert marker == bc.ChangeMarker(2, MARKER_ID.unpack(value)[0]) and marker.id != 0

    # The header's marker: the origin before the promotion, the marker from it on.
    assert marker_at(follower, 0) == ORIGIN and marker_at(follower, 1) == ORIGIN
    assert marker_at(follower, 2) == marker and marker_at(follower, 99) == marker
    assert follower.changes_since(follower.snapshot(), 2).header == bc.ChangeHeader(marker, 2)

    # The first write takes the sequence after the marker's.
    assert follower.put(b"b", b"2").sequence == 3

    # The old leader follows the new one and receives the marker as an entry.
    catch_up(follower, leader)
    assert markers(leader) == [marker] and history(leader) == history(follower)
    assert marker_at(leader, 99) == marker

    # Reopening in Leader mode is not a promotion.
    follower.close()
    reopened = bc.DB.open(str(tmp_path / "follower"))
    assert markers(reopened) == [marker] and reopened.durable_sequence() == 3
    assert reopened.put(b"c", b"3").sequence == 4
    assert markers(reopened) == [marker]
    reopened.close()
    leader.close()


def test_a_gap_is_refused_before_anything_is_written(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    nosync = bc.WriteOptions()
    nosync.sync = False
    for i in range(5):
        leader.put(bytes([97 + i]), b"v", nosync)
    leader.put(b"z", b"v")  # 6 entries, all durable
    follower = open_follower(tmp_path / "follower")
    replicate(leader, follower, max_bytes=1)  # one unit per slice
    replicate(leader, follower, max_bytes=1)
    assert follower.durable_sequence() == 2

    header, entries = slice_from(leader, 3)  # starts past the follower: 3 is missing
    assert header.from_sequence == 3 and entries[0].sequence == 4
    with pytest.raises(ValueError):
        follower.ingest(header, entries)
    assert follower.durable_sequence() == 2 and len(history(follower)) == 2
    with pytest.raises(ValueError):
        follower.ingest(bc.ChangeHeader(ORIGIN, 3), [])  # an empty slice past P is a gap too
    replicate(leader, follower)  # from 2: accepted
    assert history(follower) == history(leader)
    leader.close()
    follower.close()


def test_promoting_the_less_advanced_follower_forks_the_one_ahead(tmp_path):
    """The unplanned failover of the design: L dies at 20; N is at 10, F at 20."""
    L = bc.DB.open(str(tmp_path / "L"))
    for i in range(10):
        L.put(f"k{i:02}".encode(), b"L")
    N = bootstrap(L, tmp_path / "N")
    G = bootstrap(L, tmp_path / "G")
    for i in range(10, 20):
        L.put(f"k{i:02}".encode(), b"L")
    F = bootstrap(L, tmp_path / "F")
    assert (N.durable_sequence(), G.durable_sequence(), F.durable_sequence()) == (10, 10, 20)
    L.close()  # L dies

    N.set_mode(bc.Mode.Leader)  # the wrong choice: F is ahead of N
    N.put(b"n", b"1")
    (marker,) = markers(N)
    assert marker.since_sequence == 11
    assert marker_at(N, 20) == marker and marker_at(F, 20) == ORIGIN

    # F holds L's 11..20, which N never had: refused, and nothing is written.
    before = history(F)
    with pytest.raises(bc.DbChangeMarkerMismatch):
        replicate(N, F)
    # Re-delivered from an earlier point, the marker is in the slice: refused too.
    with pytest.raises(bc.DbChangeMarkerMismatch):
        replicate(N, F, from_sequence=5)
    assert history(F) == before and F.durable_sequence() == 20

    # G, at N's position, catches up and receives the marker.
    catch_up(N, G)
    assert markers(G) == [marker] and history(G) == history(N)

    # L comes back with writes nobody replicated: refused the same way.
    L = open_follower(tmp_path / "L")
    assert L.durable_sequence() == 20
    with pytest.raises(bc.DbChangeMarkerMismatch):
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
    for node in (N, F, L, G):
        node.close()


def test_re_delivery_of_the_same_history_is_accepted(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    for i in range(6):
        leader.put(bytes([97 + i]), b"1")
    follower = open_follower(tmp_path / "follower")
    catch_up(leader, follower)
    assert replicate(leader, follower, from_sequence=3)  # 4..6 again: all duplicates
    assert follower.durable_sequence() == 6 and len(history(follower)) == 6
    leader.put(b"g", b"1")
    leader.put(b"h", b"1")
    replicate(leader, follower, from_sequence=3)  # 4..8: duplicates skipped, 7 and 8 applied
    assert history(follower) == history(leader)

    # With a promotion in the re-delivered range.
    leader.set_mode(bc.Mode.Follower)
    follower.set_mode(bc.Mode.Leader)  # marker at 9
    follower.put(b"i", b"1")  # 10
    catch_up(follower, leader)
    assert markers(leader) == markers(follower)
    replicate(follower, leader, from_sequence=4)  # 5..10 again, marker included
    replicate(follower, leader, from_sequence=9)  # from the marker itself
    assert history(leader) == history(follower)
    leader.close()
    follower.close()


def test_max_bytes_cuts_after_a_whole_unit(tmp_path):
    opts = bc.Options()
    opts.max_file_bytes = 300
    leader = bc.DB.open(str(tmp_path / "leader"), opts)
    follower = open_follower(tmp_path / "follower", max_file_bytes=200)
    nosync = bc.WriteOptions()
    nosync.sync = False
    for i in range(30):
        leader.put(f"k{i:02}".encode(), b"v" * (i % 7), nosync)
        if i % 5 == 0:
            plan = bc.WritePlan()
            plan.put(b"b1", bytes([i]))
            plan.put(b"b2", b"x")
            plan.put(b"b3", b"y")
            assert leader.apply_batch(plan, nosync) is not None
    leader.del_(b"k03", nosync)
    leader.del_range(b"k10", b"k15", nosync)
    last = leader.put(b"last", b"!")
    assert leader.durable_sequence() == last.sequence

    slices = []
    while follower.durable_sequence() < leader.durable_sequence():
        slices.append(replicate(leader, follower, max_bytes=120))
    assert len(slices) > 5
    for entries in slices:
        assert entries, "behind the leader, a slice is never empty"
        sequences = [e.sequence for e in entries]
        assert sequences == sorted(set(sequences))
        begins = sum(e.entry_type == T.BulkBegin for e in entries)
        assert begins == sum(e.entry_type == T.BulkEnd for e in entries), "cut inside a batch"
        assert entries[-1].entry_type != T.BulkBegin
    assert any(e.entry_type == T.RangeDel for s in slices for e in s)

    # max_bytes=1 gives one unit per slice: a whole batch, or one entry.
    units = []
    header, entries = slice_from(leader, 0, max_bytes=1)
    while entries:
        units.append(entries)
        header, entries = slice_from(leader, entries[-1].sequence, max_bytes=1)
    kinds = [[e.entry_type for e in u] for u in units]
    assert all(k == [T.BulkBegin, T.Put, T.Put, T.Put, T.BulkEnd] or len(k) == 1 for k in kinds)
    assert sum(len(u) for u in units) == len(history(leader))

    assert list(follower.iter_from()) == list(leader.iter_from())
    assert history(follower) == history(leader)
    assert replicate(leader, follower) == []  # caught up: an empty slice is accepted
    follower.close()
    follower = open_follower(tmp_path / "follower", max_file_bytes=200)
    assert list(follower.iter_from()) == list(leader.iter_from())
    assert follower.durable_sequence() == leader.durable_sequence()
    follower.close()
    leader.close()


def test_data_entry_change_marker_round_trips_through_ingest(tmp_path):
    follower = open_follower(tmp_path / "follower")
    id_bytes = MARKER_ID.pack(0xDEADBEEFCAFEF00D)
    entries = [
        bc.DataEntry(1, T.Put, b"a", b"1"),
        bc.DataEntry(2, T.ChangeMarker, b"", id_bytes),
        bc.DataEntry(3, T.Put, b"b", b"2"),
    ]
    follower.ingest(bc.ChangeHeader(ORIGIN, 0), entries)
    assert follower.durable_sequence() == 3
    assert history(follower)[1] == (2, T.ChangeMarker, b"", id_bytes)
    assert markers(follower) == [bc.ChangeMarker(2, 0xDEADBEEFCAFEF00D)]
    assert marker_at(follower, 1) == ORIGIN
    assert marker_at(follower, 3) == bc.ChangeMarker(2, 0xDEADBEEFCAFEF00D)

    # Malformed markers are refused before anything is written.
    for bad in ([bc.DataEntry(4, T.ChangeMarker, b"k", id_bytes)],
                [bc.DataEntry(4, T.ChangeMarker, b"", b"short")],
                [bc.DataEntry(4, T.BulkBegin, b"", b""),
                 bc.DataEntry(5, T.ChangeMarker, b"", id_bytes),
                 bc.DataEntry(6, T.BulkEnd, b"", b"")]):
        with pytest.raises(ValueError):
            follower.ingest(bc.ChangeHeader(bc.ChangeMarker(2, 0xDEADBEEFCAFEF00D), 3), bad)
        assert follower.durable_sequence() == 3

    # A slice whose header names another history at the same point is a fork.
    with pytest.raises(bc.DbChangeMarkerMismatch):
        follower.ingest(bc.ChangeHeader(bc.ChangeMarker(2, 1), 3), [bc.DataEntry(4, T.Put, b"c", b"3")])
    assert follower.durable_sequence() == 3

    # Survives a reopen.
    follower.close()
    follower = open_follower(tmp_path / "follower")
    assert markers(follower) == [bc.ChangeMarker(2, 0xDEADBEEFCAFEF00D)]
    follower.close()


def test_vacuum_retain_after_keeps_history(tmp_path):
    opts = bc.Options()
    opts.max_file_bytes = 128
    db = bc.DB.open(str(tmp_path / "retained"), opts)
    vopts = bc.VacuumOptions()
    assert vopts.retain_after == -1  # no restriction by default
    for i in range(10):
        db.put(f"k{i}".encode(), b"old")
    retain = db.durable_sequence()
    for i in range(10):
        db.put(f"k{i}".encode(), b"new")
    vopts.fragmentation_threshold = 0.0
    vopts.retain_after = retain
    while db.vacuum(vopts):
        pass
    snap = db.snapshot()
    seqs = [e.sequence for e in db.changes_since(snap, retain).entries]
    assert seqs == list(range(retain + 1, db.durable_sequence() + 1))
    with pytest.raises(ValueError):
        vopts.retain_after = -2
