import bytecaskdb._bytecaskdb as bc


def test_forward_iteration(populated_db):
    items = list(populated_db.iter_from())
    assert len(items) == 5
    assert items[0] == (b"user:1", b"name_1")
    assert items[4] == (b"user:5", b"name_5")


def test_forward_from_key(populated_db):
    items = list(populated_db.iter_from(b"user:3"))
    assert len(items) == 3
    assert items[0] == (b"user:3", b"name_3")


def test_keys_only(populated_db):
    keys = list(populated_db.keys_from())
    assert keys == [f"user:{i}".encode() for i in range(1, 6)]


def test_keys_from_prefix(populated_db):
    keys = list(populated_db.keys_from(b"user:3"))
    assert keys == [b"user:3", b"user:4", b"user:5"]


def test_reverse_iteration(populated_db):
    items = list(populated_db.riter_from())
    assert len(items) == 5
    assert items[0] == (b"user:5", b"name_5")
    assert items[4] == (b"user:1", b"name_1")


def test_reverse_from_key(populated_db):
    items = list(populated_db.riter_from(b"user:3"))
    assert len(items) == 3
    assert items[0] == (b"user:3", b"name_3")
    assert items[2] == (b"user:1", b"name_1")


def test_reverse_keys(populated_db):
    keys = list(populated_db.rkeys_from())
    assert keys == [f"user:{i}".encode() for i in range(5, 0, -1)]


def test_reverse_keys_from(populated_db):
    keys = list(populated_db.rkeys_from(b"user:3"))
    assert keys == [b"user:3", b"user:2", b"user:1"]


def test_empty_db_iteration(db):
    assert list(db.iter_from()) == []
    assert list(db.keys_from()) == []
    assert list(db.riter_from()) == []
    assert list(db.rkeys_from()) == []


def test_iteration_past_end(populated_db):
    items = list(populated_db.iter_from(b"zzz"))
    assert items == []
