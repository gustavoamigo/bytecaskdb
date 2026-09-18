import pytest
import bytecaskdb._bytecaskdb as bc


def test_mmap_io_backend_roundtrips(tmp_path):
    opts = bc.Options()
    opts.io_backend = bc.IoBackend.Mmap
    db = bc.DB.open(str(tmp_path / "mmap_db"), opts)
    db.put(b"k1", b"v1")
    assert db.get(b"k1") == b"v1"


def test_buffer_pool_requires_capacity_at_least_2x_max_file_bytes(tmp_path):
    opts = bc.Options()
    opts.max_file_bytes = 4 * 1024 * 1024
    opts.io_backend = bc.IoBackend.BufferPool
    opts.buffer_pool.capacity_bytes = 1024 * 1024
    with pytest.raises(ValueError):
        bc.DB.open(str(tmp_path / "pool_db"), opts)


def test_buffer_pool_roundtrips_and_reports_stats(tmp_path):
    max_file_bytes = 4 * 1024 * 1024
    opts = bc.Options()
    opts.max_file_bytes = max_file_bytes
    opts.io_backend = bc.IoBackend.BufferPool
    opts.buffer_pool.capacity_bytes = 2 * max_file_bytes
    opts.buffer_pool.direct_io = False
    db = bc.DB.open(str(tmp_path / "pool_db"), opts)

    for i in range(50):
        db.put(f"key-{i}".encode(), f"value-{i}".encode())
    for i in range(50):
        assert db.get(f"key-{i}".encode()) == f"value-{i}".encode()

    stats = db.stats()
    assert isinstance(stats, dict)
    for counter in ("bytecask.pool_hits", "bytecask.pool_misses", "bytecask.pool_frames_total"):
        assert counter in stats


def test_stats_returns_dict(db):
    stats = db.stats()
    assert isinstance(stats, dict)
    assert "bytecask.bytes_written" in stats
    db.put(b"k1", b"v1")
    assert db.stats()["bytecask.bytes_written"] > stats["bytecask.bytes_written"]
