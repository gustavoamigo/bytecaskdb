import gc
import bytecaskdb as bc


def test_snapshot_released_after_with(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"k1", b"v1", opts)
    with db.snapshot() as snap:
        assert snap.get(b"k1") == b"v1"
    # Snapshot should be GC-eligible after exiting the with block.
    # Verify vacuum can run (it would be blocked by a lingering snapshot
    # holding old file references).
    gc.collect()
    db.vacuum()


def test_iterator_released_after_loop(db):
    opts = bc.WriteOptions()
    opts.sync = False
    for i in range(10):
        db.put(f"k{i:03d}".encode(), b"v", opts)

    # Iterate fully — iterator should be released
    keys = list(db.keys_from())
    assert len(keys) == 10

    gc.collect()
    db.vacuum()


def test_many_snapshots_released(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"k", b"v", opts)

    # Create and drop many snapshots
    for _ in range(100):
        snap = db.snapshot()
        assert snap.get(b"k") == b"v"
        del snap

    gc.collect()
    db.vacuum()
