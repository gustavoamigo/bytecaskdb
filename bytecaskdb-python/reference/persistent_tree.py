# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Gustavo Amigo

"""An immutable sorted map: a persistent binary search tree.

A tree is never changed. ``set``, ``remove`` and ``discard`` return a new tree
that shares every node off the path to the change with the old one, so keeping
an old tree costs nothing and it goes on describing the map as it was::

    v1 = PersistentTree().set("b", 2).set("a", 1)
    v2 = v1.set("c", 3).remove("a")
    list(v1.items())  # [('a', 1), ('b', 2)]
    list(v2.items())  # [('b', 2), ('c', 3)]

Keys may be of any mutually comparable type. The tree is not balanced:
inserting keys in sorted order degrades it to a list, and every operation to
O(n). No operation recurses, so that costs time, not a RecursionError.
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from typing import Any, Generic, TypeVar

K = TypeVar("K")
V = TypeVar("V")


class _Node:
    __slots__ = ("key", "value", "left", "right")

    def __init__(self, key: Any, value: Any, left: _Node | None, right: _Node | None):
        self.key = key
        self.value = value
        self.left = left
        self.right = right


# The nodes from the root down to a point in the tree, each with the side the
# path left it by.
_Path = list[tuple[_Node, bool]]


def _find(root: _Node | None, key: Any) -> tuple[_Path, _Node | None]:
    """The path to key, and its node if present."""
    path: _Path = []
    node = root
    while node is not None and key != node.key:
        went_left = key < node.key
        path.append((node, went_left))
        node = node.left if went_left else node.right
    return path, node


def _copy_path(path: _Path, child: _Node | None) -> _Node | None:
    """Copies the path bottom-up with child where it ended; returns the new root."""
    for node, went_left in reversed(path):
        if went_left:
            child = _Node(node.key, node.value, child, node.right)
        else:
            child = _Node(node.key, node.value, node.left, child)
    return child


class PersistentTree(Mapping[K, V], Generic[K, V]):
    __slots__ = ("_root", "_len")

    def __init__(self) -> None:
        self._root: _Node | None = None
        self._len = 0

    @classmethod
    def _make(cls, root: _Node | None, length: int) -> PersistentTree[K, V]:
        tree = cls.__new__(cls)
        tree._root = root
        tree._len = length
        return tree

    # --- Mapping -------------------------------------------------------------

    def __getitem__(self, key: K) -> V:
        _, node = _find(self._root, key)
        if node is None:
            raise KeyError(key)
        return node.value

    def __len__(self) -> int:
        return self._len

    def __iter__(self) -> Iterator[K]:
        for key, _ in self.ascending():
            yield key

    def __repr__(self) -> str:
        return f"PersistentTree({dict(self.ascending())!r})"

    # --- Updates: each returns a new tree ------------------------------------

    def set(self, key: K, value: V) -> PersistentTree[K, V]:
        """The tree with key mapped to value."""
        path, node = _find(self._root, key)
        if node is None:
            return self._make(_copy_path(path, _Node(key, value, None, None)), self._len + 1)
        return self._make(_copy_path(path, _Node(key, value, node.left, node.right)), self._len)

    def remove(self, key: K) -> PersistentTree[K, V]:
        """The tree without key. Raises KeyError if it is absent."""
        path, node = _find(self._root, key)
        if node is None:
            raise KeyError(key)
        if node.left is None:
            replacement = node.right
        elif node.right is None:
            replacement = node.left
        else:
            # Two children: the successor, the leftmost node on the right, takes its place.
            succ_path: _Path = []
            succ = node.right
            while succ.left is not None:
                succ_path.append((succ, True))
                succ = succ.left
            replacement = _Node(succ.key, succ.value, node.left, _copy_path(succ_path, succ.right))
        return self._make(_copy_path(path, replacement), self._len - 1)

    def discard(self, key: K) -> PersistentTree[K, V]:
        """The tree without key; this tree if it is absent."""
        return self.remove(key) if key in self else self

    # --- Ordered scans -------------------------------------------------------

    def ascending(self, start: K | None = None, stop: K | None = None) -> Iterator[tuple[K, V]]:
        """Items with start <= key < stop in ascending order; a bound left as None is open."""
        stack: list[_Node] = []
        node = self._root
        while node is not None:  # down to start, keeping the nodes to visit
            if start is None or node.key >= start:
                stack.append(node)
                node = node.left
            else:
                node = node.right
        while stack:
            node = stack.pop()
            if stop is not None and node.key >= stop:
                return
            yield node.key, node.value
            node = node.right
            while node is not None:
                stack.append(node)
                node = node.left

    def descending(self, start: K | None = None) -> Iterator[tuple[K, V]]:
        """Items with key <= start in descending order; every item if start is None."""
        stack: list[_Node] = []
        node = self._root
        while node is not None:
            if start is None or node.key <= start:
                stack.append(node)
                node = node.right
            else:
                node = node.left
        while stack:
            node = stack.pop()
            yield node.key, node.value
            node = node.left
            while node is not None:
                stack.append(node)
                node = node.right
