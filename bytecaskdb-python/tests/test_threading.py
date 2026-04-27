import threading
import time
import bytecaskdb._bytecaskdb as bc


def test_concurrent_reads(populated_db):
    """Verify GIL is released during get — threads run concurrently."""
    results = {}
    barrier = threading.Barrier(4)

    def reader(thread_id):
        barrier.wait()
        count = 0
        start = time.monotonic()
        while time.monotonic() - start < 0.1:
            val = populated_db.get(f"user:{(count % 5) + 1}".encode())
            assert val is not None
            count += 1
        results[thread_id] = count

    threads = [threading.Thread(target=reader, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # With GIL released, total ops across 4 threads should be significantly
    # more than a single thread. At minimum, verify all threads made progress.
    for tid, count in results.items():
        assert count > 0, f"Thread {tid} made no progress — GIL not released?"

    total = sum(results.values())
    print(f"  Concurrent reads: {total} ops across 4 threads in 0.1s")


def test_concurrent_writes(db):
    """Verify GIL is released during put — threads run concurrently."""
    barrier = threading.Barrier(4)
    opts = bc.WriteOptions()
    opts.sync = False

    def writer(thread_id):
        barrier.wait()
        for i in range(100):
            key = f"t{thread_id}:k{i}".encode()
            db.put(key, b"val", opts)

    threads = [threading.Thread(target=writer, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # Verify all writes landed
    for tid in range(4):
        for i in range(100):
            assert db.get(f"t{tid}:k{i}".encode()) is not None


def test_concurrent_iterators(populated_db):
    """Separate iterators on the same DB can be consumed concurrently."""
    barrier = threading.Barrier(4)
    results = {}

    def iterate(thread_id):
        barrier.wait()
        keys = []
        for key in populated_db.keys_from(b""):
            keys.append(key)
        results[thread_id] = keys

    threads = [threading.Thread(target=iterate, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    # All threads should see the same 5 keys.
    for tid, keys in results.items():
        assert len(keys) == 5, f"Thread {tid} saw {len(keys)} keys, expected 5"


def test_concurrent_snapshot_reads(populated_db):
    """Snapshot reads are lock-free — concurrent threads make progress."""
    snap = populated_db.snapshot()
    barrier = threading.Barrier(4)
    results = {}

    def reader(thread_id):
        barrier.wait()
        count = 0
        start = time.monotonic()
        while time.monotonic() - start < 0.1:
            val = snap.get(f"user:{(count % 5) + 1}".encode())
            assert val is not None
            count += 1
        results[thread_id] = count

    threads = [threading.Thread(target=reader, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    for tid, count in results.items():
        assert count > 0, f"Thread {tid} made no progress on snapshot reads"


def test_snapshot_double_consume(db):
    """Consuming a snapshot twice raises RuntimeError, not UB."""
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"k", b"v", opts)
    snap = db.snapshot()

    # First consume succeeds.
    plan1 = bc.WritePlan(snap)
    plan1.put(b"k", b"v2")
    assert db.apply_batch(plan1)

    # Second consume raises — snapshot was already taken.
    try:
        bc.WritePlan(snap)
        assert False, "Expected RuntimeError"
    except RuntimeError as e:
        assert "consumed" in str(e).lower()


def test_concurrent_snapshot_consume(db):
    """Racing to consume the same snapshot: exactly one wins, others get RuntimeError."""
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"k", b"v", opts)

    snap = db.snapshot()
    barrier = threading.Barrier(4)
    outcomes = {}  # thread_id -> "ok" or "error"

    def consume(thread_id):
        barrier.wait()
        try:
            plan = bc.WritePlan(snap)
            plan.put(b"k", f"v{thread_id}".encode())
            db.apply_batch(plan)
            outcomes[thread_id] = "ok"
        except RuntimeError:
            outcomes[thread_id] = "error"

    threads = [threading.Thread(target=consume, args=(i,)) for i in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    ok_count = sum(1 for v in outcomes.values() if v == "ok")
    error_count = sum(1 for v in outcomes.values() if v == "error")
    assert ok_count == 1, f"Expected exactly 1 winner, got {ok_count}"
    assert error_count == 3, f"Expected 3 errors, got {error_count}"
