import bytecaskdb as bc


def test_snapshot_isolation(db):
    db.put(b"k1", b"v1")
    snap = db.snapshot()

    # Write after snapshot
    db.put(b"k1", b"v2")
    db.put(b"k2", b"new")

    # Snapshot sees old state
    assert snap.get(b"k1") == b"v1"
    assert snap.get(b"k2") is None

    # DB sees new state
    assert db.get(b"k1") == b"v2"
    assert db.get(b"k2") == b"new"


def test_snapshot_context_manager(db):
    db.put(b"k1", b"v1")
    with db.snapshot() as snap:
        assert snap.get(b"k1") == b"v1"
        assert snap.contains_key(b"k1") is True
        assert snap.contains_key(b"missing") is False


def test_snapshot_get_missing(db):
    snap = db.snapshot()
    assert snap.get(b"missing") is None


def test_snapshot_iter(populated_db):
    snap = populated_db.snapshot()
    items = list(snap.iter_from(b"user:"))
    assert len(items) == 5
    assert items[0] == (b"user:1", b"name_1")


def test_snapshot_keys(populated_db):
    snap = populated_db.snapshot()
    keys = list(snap.keys_from(b"user:"))
    assert keys == [f"user:{i}".encode() for i in range(1, 6)]
