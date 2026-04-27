import pytest
import bytecaskdb._bytecaskdb as bc


def test_db_degraded_is_runtime_error():
    assert issubclass(bc.DbDegraded, RuntimeError)


def test_logic_error_becomes_value_error(db):
    plan = bc.WritePlan()
    with pytest.raises(ValueError):
        plan.ensure_unchanged(b"key")


def test_open_bad_path_raises():
    with pytest.raises(Exception):
        bc.DB.open("/proc/nonexistent/impossible/path")


def test_is_degraded_initially_false(db):
    assert db.is_degraded is False
    assert db.degraded_reason == ""
