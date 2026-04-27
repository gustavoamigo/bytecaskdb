import pytest
import bytecaskdb._bytecaskdb as bc


@pytest.fixture
def db(tmp_path):
    return bc.DB.open(str(tmp_path / "testdb"))


@pytest.fixture
def nosync():
    opts = bc.WriteOptions()
    opts.sync = False
    return opts


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


def test_current_sequence_empty(db):
    assert db.current_sequence() == 0


def test_current_sequence_increments(db):
    opts = bc.WriteOptions()
    opts.sync = True
    db.put(b"a", b"1", opts)
    seq1 = db.current_sequence()
    assert seq1 > 0
    db.put(b"b", b"2", opts)
    seq2 = db.current_sequence()
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
    entries = list(db.changes_since(snap, 0))
    assert len(entries) >= 3

    # Entries are in sequence order.
    sequences = [e.sequence for e in entries]
    assert sequences == sorted(sequences)

    # Contains our puts.
    put_entries = [e for e in entries if e.entry_type == bc.EntryType.Put]
    keys = {bytes(e.key) for e in put_entries}
    assert b"a" in keys
    assert b"b" in keys
    assert b"c" in keys


def test_changes_since_from_sequence(db):
    opts = bc.WriteOptions()
    opts.sync = True
    db.put(b"a", b"1", opts)
    seq_after_a = db.current_sequence()
    db.put(b"b", b"2", opts)

    snap = db.snapshot()
    entries = list(db.changes_since(snap, seq_after_a))
    put_entries = [e for e in entries if e.entry_type == bc.EntryType.Put]
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

    snap = leader.snapshot()
    entries = list(leader.changes_since(snap, 0))

    # Follower ingests.
    follower_opts = bc.Options()
    follower_opts.initial_mode = bc.Mode.Follower
    follower = bc.DB.open(str(tmp_path / "follower"), follower_opts)
    assert follower.mode == bc.Mode.Follower
    follower.ingest(entries)

    assert follower.get(b"k1") == b"v1"
    assert follower.get(b"k2") == b"v2"


def test_ingest_idempotency(tmp_path):
    leader = bc.DB.open(str(tmp_path / "leader"))
    opts = bc.WriteOptions()
    opts.sync = True
    leader.put(b"k1", b"v1", opts)

    snap = leader.snapshot()
    entries = list(leader.changes_since(snap, 0))

    follower_opts = bc.Options()
    follower_opts.initial_mode = bc.Mode.Follower
    follower = bc.DB.open(str(tmp_path / "follower"), follower_opts)
    follower.ingest(entries)
    # Re-ingest same entries — should be idempotent.
    follower.ingest(entries)
    assert follower.get(b"k1") == b"v1"


def test_leader_to_follower_replication(tmp_path):
    """End-to-end: leader writes, follower replicates via changes_since + ingest."""
    leader = bc.DB.open(str(tmp_path / "leader"))
    follower_opts = bc.Options()
    follower_opts.initial_mode = bc.Mode.Follower
    follower = bc.DB.open(str(tmp_path / "follower"), follower_opts)

    follower_seq = 0
    opts = bc.WriteOptions()
    opts.sync = True

    # Round 1: initial writes.
    leader.put(b"user:1", b"alice", opts)
    leader.put(b"user:2", b"bob", opts)

    snap = leader.snapshot()
    changes = list(leader.changes_since(snap, follower_seq))
    follower.ingest(changes)
    follower_seq = follower.current_sequence()

    assert follower.get(b"user:1") == b"alice"
    assert follower.get(b"user:2") == b"bob"

    # Round 2: incremental replication.
    leader.put(b"user:3", b"carol", opts)
    leader.del_(b"user:1", opts)

    snap2 = leader.snapshot()
    changes2 = list(leader.changes_since(snap2, follower_seq))
    follower.ingest(changes2)

    assert follower.get(b"user:1") is None
    assert follower.get(b"user:2") == b"bob"
    assert follower.get(b"user:3") == b"carol"


def test_entry_type_enum_values():
    assert bc.EntryType.Put is not None
    assert bc.EntryType.Delete is not None
    assert bc.EntryType.BulkBegin is not None
    assert bc.EntryType.BulkEnd is not None
    assert bc.EntryType.RangeDel is not None


def test_mode_enum_values():
    assert bc.Mode.Leader is not None
    assert bc.Mode.Follower is not None
