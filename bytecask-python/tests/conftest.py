import pytest
import bytecaskdb as bc


@pytest.fixture
def db(tmp_path):
    """Create a fresh DB in a temporary directory."""
    return bc.DB.open(str(tmp_path / "testdb"))


@pytest.fixture
def populated_db(db):
    """DB pre-populated with user:1..user:5."""
    opts = bc.WriteOptions()
    opts.sync = False
    for i in range(1, 6):
        db.put(f"user:{i}".encode(), f"name_{i}".encode(), opts)
    return db
