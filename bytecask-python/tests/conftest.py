import pytest
from bytecaskdb._bytecaskdb import DB as _RawDB
from bytecaskdb._bytecaskdb import WriteOptions


@pytest.fixture
def db(tmp_path):
    """Create a fresh DB in a temporary directory (low-level C API)."""
    return _RawDB.open(str(tmp_path / "testdb"))


@pytest.fixture
def populated_db(db):
    """DB pre-populated with user:1..user:5."""
    opts = WriteOptions()
    opts.sync = False
    for i in range(1, 6):
        db.put(f"user:{i}".encode(), f"name_{i}".encode(), opts)
    return db
