# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo
#
# The reference implementation's building blocks: the persistent tree and
# CRC-32C (reference/persistent_tree.py, reference/checksum.py).

import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "reference"))
from checksum import crc32c  # noqa: E402
from persistent_tree import PersistentTree  # noqa: E402


def test_crc32c_check_value():
    assert crc32c(b"123456789") == 0xE3069283


def test_mapping_interface():
    t = PersistentTree().set("b", 2).set("a", 1).set("c", 3)
    assert len(t) == 3 and t["a"] == 1 and t.get("z") is None
    assert "b" in t and "z" not in t
    assert list(t) == ["a", "b", "c"]
    assert t == {"a": 1, "b": 2, "c": 3}
    assert t.set("a", 9)["a"] == 9 and len(t.set("a", 9)) == 3
    with pytest.raises(KeyError):
        t.remove("z")
    assert t.discard("z") is t
    assert not PersistentTree()


def test_every_version_survives():
    rng = random.Random(7)
    versions = [(PersistentTree(), {})]
    for _ in range(2000):
        tree, model = versions[-1]
        key = rng.randrange(200)
        if rng.random() < 0.4 and key in model:
            tree = tree.remove(key)
            model = {k: v for k, v in model.items() if k != key}
        else:
            tree = tree.set(key, rng.random())
            model = {**model, key: tree[key]}
        versions.append((tree, model))
    for tree, model in versions:
        assert list(tree.items()) == sorted(model.items())


def test_ordered_scans():
    t = PersistentTree()
    for k in [50, 20, 80, 10, 30, 70, 90, 25]:
        t = t.set(k, str(k))
    assert [k for k, _ in t.ascending(21)] == [25, 30, 50, 70, 80, 90]
    assert [k for k, _ in t.ascending(20, 70)] == [20, 25, 30, 50]
    assert [k for k, _ in t.ascending(stop=25)] == [10, 20]
    assert [k for k, _ in t.descending(69)] == [50, 30, 25, 20, 10]
    assert [k for k, _ in t.descending()] == [90, 80, 70, 50, 30, 25, 20, 10]
    assert list(t.ascending(91)) == [] and list(t.descending(9)) == []


def test_sorted_inserts_do_not_recurse():
    t = PersistentTree()
    for k in range(5000):
        t = t.set(k, k)
    assert len(t) == 5000 and t[4999] == 4999
    assert next(t.descending())[0] == 4999
