import bytecaskdb._bytecaskdb as bc


def test_put_and_get(db):
    db.put(b"k1", b"v1")
    assert db.get(b"k1") == b"v1"


def test_get_missing_returns_none(db):
    assert db.get(b"nonexistent") is None


def test_put_overwrites(db):
    db.put(b"k1", b"v1")
    db.put(b"k1", b"v2")
    assert db.get(b"k1") == b"v2"


def test_del_returns_true_if_existed(db):
    db.put(b"k1", b"v1")
    assert db.del_(b"k1") is not None
    assert db.get(b"k1") is None


def test_del_returns_false_if_absent(db):
    assert db.del_(b"missing") is None


def test_contains_key(db):
    db.put(b"k1", b"v1")
    assert db.contains_key(b"k1") is True
    assert db.contains_key(b"missing") is False


def test_empty_key_and_value(db):
    db.put(b"", b"")
    assert db.get(b"") == b""


def test_large_value(db):
    big = b"x" * 1_000_000
    db.put(b"big", big)
    assert db.get(b"big") == big


def test_write_options_nosync(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"fast", b"write", opts)
    assert db.get(b"fast") == b"write"


def test_options_defaults():
    opts = bc.Options()
    assert opts.max_file_bytes == 64 * 1024 * 1024
    assert opts.recovery_threads == 4
    assert opts.fail_recovery_on_crc_errors is True


def test_write_options_defaults():
    opts = bc.WriteOptions()
    assert opts.sync is True
    assert opts.solo is False


def test_vacuum(db):
    opts = bc.WriteOptions()
    opts.sync = False
    for i in range(100):
        db.put(f"k{i}".encode(), b"v" * 100, opts)
    for i in range(100):
        db.del_(f"k{i}".encode(), opts)
    # vacuum may or may not find a file to compact
    result = db.vacuum()
    assert isinstance(result, bool)
