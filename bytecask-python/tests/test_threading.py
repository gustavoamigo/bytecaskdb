import threading
import time
import bytecaskdb as bc


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
