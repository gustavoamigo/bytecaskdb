import bytecaskdb as bc


def test_db_del_range(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"a", b"1", opts)
    db.put(b"b", b"2", opts)
    db.put(b"c", b"3", opts)
    db.put(b"d", b"4", opts)

    db.del_range(b"b", b"d")

    assert db.get(b"a") == b"1"
    assert db.get(b"b") is None
    assert db.get(b"c") is None
    assert db.get(b"d") == b"4"


def test_db_del_range_all(db):
    opts = bc.WriteOptions()
    opts.sync = False
    for i in range(10):
        db.put(f"k{i}".encode(), f"v{i}".encode(), opts)

    db.del_range(b"k", b"l")  # all k* keys

    for i in range(10):
        assert db.get(f"k{i}".encode()) is None


def test_db_del_range_noop(db):
    db.put(b"a", b"1")
    # from >= to is a no-op
    db.del_range(b"z", b"a")
    assert db.get(b"a") == b"1"


def test_db_del_range_with_write_options(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"x", b"1", opts)
    db.put(b"y", b"2", opts)

    db.del_range(b"x", b"z", opts)
    assert db.get(b"x") is None
    assert db.get(b"y") is None


def test_batch_del_range(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"a", b"1", opts)
    db.put(b"b", b"2", opts)
    db.put(b"c", b"3", opts)

    batch = bc.Batch()
    batch.put(b"d", b"4")
    batch.del_range(b"a", b"c")  # deletes a, b
    db.apply_batch(batch)

    assert db.get(b"a") is None
    assert db.get(b"b") is None
    assert db.get(b"c") == b"3"
    assert db.get(b"d") == b"4"


def test_write_plan_del_range(db):
    opts = bc.WriteOptions()
    opts.sync = False
    db.put(b"item:1", b"a", opts)
    db.put(b"item:2", b"b", opts)
    db.put(b"item:3", b"c", opts)

    snap = db.snapshot()
    plan = bc.WritePlan(snap)
    plan.del_range(b"item:1", b"item:3")  # deletes item:1, item:2
    plan.put(b"summary", b"done")

    assert db.apply_batch_if(plan) is True
    assert db.get(b"item:1") is None
    assert db.get(b"item:2") is None
    assert db.get(b"item:3") == b"c"
    assert db.get(b"summary") == b"done"


def test_write_plan_del_range_no_snapshot(db):
    plan = bc.WritePlan()
    plan.del_range(b"a", b"z")
    plan.put(b"new", b"val")
    assert db.apply_batch_if(plan) is True
    assert db.get(b"new") == b"val"
