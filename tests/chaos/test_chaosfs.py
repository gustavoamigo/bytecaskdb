# SPDX-License-Identifier: MIT
"""Tests for chaosfs's model: the rig is only as sound as its filesystem, so
the rules the oracle relies on are checked here, without FUSE.

Run: python3 -m unittest tests/chaos/test_chaosfs.py
"""

import errno
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from chaosfs import PAGE, SECTOR, ChaosModel  # noqa: E402


def new_file(m: ChaosModel, path: str, data: bytes = b"") -> int:
    fh = m.create(path, 0o644, os.O_RDWR)
    if data:
        m.write(path, data, 0, fh)
    return fh


def crash(m: ChaosModel, seed: int = 0, subset: bool = False):
    m.freeze()
    summary = m.crash(seed, subset)
    m.unfence()
    return summary


def content(m: ChaosModel, path: str) -> bytes:
    fh = m.open(path, os.O_RDONLY)
    try:
        return m.read(path, 1 << 30, 0, fh)
    finally:
        m.release(fh)


class DurabilityTest(unittest.TestCase):
    def test_synced_file_and_entry_survive_every_crash(self):
        for seed in range(50):
            m = ChaosModel(seed)
            fh = new_file(m, "/a", b"x" * 10000)
            m.fsync("/a", 1, fh)
            m.fsyncdir("/", None)
            crash(m, seed, subset=seed % 2 == 0)
            self.assertEqual(content(m, "/a"), b"x" * 10000)

    def test_unsynced_entry_may_vanish_synced_data_not_enough(self):
        vanished = kept = 0
        for seed in range(100):
            m = ChaosModel(seed)
            fh = new_file(m, "/a", b"y" * 100)
            m.fsync("/a", 1, fh)  # data durable, name not
            crash(m, seed)
            try:
                self.assertEqual(content(m, "/a"), b"y" * 100)
                kept += 1
            except OSError as e:
                self.assertEqual(e.errno, errno.ENOENT)
                vanished += 1
        self.assertGreater(vanished, 0)
        self.assertGreater(kept, 0)

    def test_unsynced_pages_are_old_new_or_torn_at_sectors(self):
        seen = set()
        for seed in range(200):
            m = ChaosModel(seed)
            fh = new_file(m, "/a", b"o" * (2 * PAGE))
            m.fsync("/a", 1, fh)
            m.fsyncdir("/", None)
            m.write("/a", b"n" * (2 * PAGE), 0, fh)
            crash(m, seed)
            got = content(m, "/a")
            self.assertEqual(len(got), 2 * PAGE)
            for s in range(0, len(got), SECTOR):
                sector = got[s:s + SECTOR]
                self.assertIn(sector, (b"o" * SECTOR, b"n" * SECTOR))
            for p in range(2):
                page = got[p * PAGE:(p + 1) * PAGE]
                seen.add("old" if page == b"o" * PAGE else
                         "new" if page == b"n" * PAGE else "torn")
        self.assertEqual(seen, {"old", "new", "torn"})

    def test_rename_is_whole_or_absent(self):
        for seed in range(100):
            m = ChaosModel(seed)
            fh = new_file(m, "/t", b"hint")
            m.fsync("/t", 1, fh)
            m.release(fh)
            m.fsyncdir("/", None)
            m.rename("/t", "/h")
            crash(m, seed, subset=seed % 2 == 1)
            names = set(m.readdir("/")) - {".", ".."}
            self.assertIn(names, ({"t"}, {"h"}))

    def test_random_ops_never_lose_synced_state(self):
        # Random writes and truncates, then fsync every file and the
        # directory, then more unsynced changes to some files. After a crash,
        # every file untouched since the sync reads back exactly as synced.
        for seed in range(40):
            rng = random.Random(seed)
            m = ChaosModel(seed)
            names = [f"/f{i}" for i in range(4)]
            fhs = {n: new_file(m, n) for n in names}

            def mutate(n):
                if rng.random() < 0.8:
                    off = rng.randrange(0, 3 * PAGE)
                    m.write(n, bytes([rng.randrange(256)]) * rng.randrange(1, 3000),
                            off, fhs[n])
                else:
                    m.truncate(n, rng.randrange(0, 3 * PAGE), fhs[n])

            for _ in range(100):
                mutate(rng.choice(names))
            for n in names:
                m.fsync(n, 1, fhs[n])
            m.fsyncdir("/", None)
            synced = {n: content(m, n) for n in names}
            touched = set(rng.sample(names, 2))
            for _ in range(50):
                mutate(rng.choice(sorted(touched)))
            crash(m, seed, subset=seed % 3 == 0)
            for n in names:
                if n not in touched:
                    self.assertEqual(content(m, n), synced[n], f"seed {seed} {n}")


class FsyncErrorTest(unittest.TestCase):
    def test_failed_fsync_is_reported_once_then_succeeds_without_writing(self):
        m = ChaosModel(1)
        fh = new_file(m, "/a", b"A" * PAGE)
        m.fsync("/a", 1, fh)
        m.fsyncdir("/", None)
        m.write("/a", b"B" * PAGE, PAGE, fh)
        m.set_faults({"fsync_eio": 1.0}, 7)
        with self.assertRaises(OSError) as e:
            m.fsync("/a", 1, fh)
        self.assertEqual(e.exception.errno, errno.EIO)
        m.set_faults({}, None)
        m.fsync("/a", 1, fh)  # nothing dirty: succeeds
        # The cache still has B...
        self.assertEqual(content(m, "/a"), b"A" * PAGE + b"B" * PAGE)
        lost_any = False
        for seed in range(20):
            m2 = ChaosModel(seed)
            fh2 = new_file(m2, "/a", b"A" * PAGE)
            m2.fsync("/a", 1, fh2)
            m2.fsyncdir("/", None)
            m2.write("/a", b"B" * PAGE, PAGE, fh2)
            m2.set_faults({"fsync_eio": 1.0}, seed)
            with self.assertRaises(OSError):
                m2.fsync("/a", 1, fh2)
            m2.set_faults({}, None)
            m2.fsync("/a", 1, fh2)
            m2.evict()
            got = content(m2, "/a")
            self.assertEqual(got[:PAGE], b"A" * PAGE)
            if got[PAGE:] != b"B" * PAGE:
                lost_any = True
        self.assertTrue(lost_any, "eviction never exposed a lost page")

    def test_error_reported_to_a_handle_opened_before_it(self):
        m = ChaosModel(2)
        fh1 = new_file(m, "/a", b"x" * 10)
        fh2 = m.open("/a", os.O_RDWR)
        m.writeback(seed=3, fail=True)
        # Background writeback of a half of dirty pages: may touch none.
        if m.stats()["failed_pages"]:
            with self.assertRaises(OSError):
                m.fsync("/a", 1, fh1)
            with self.assertRaises(OSError):
                m.fsync("/a", 1, fh2)
            m.fsync("/a", 1, fh1)


class SpaceTest(unittest.TestCase):
    def test_extent_model_short_write_then_enospc(self):
        m = ChaosModel(0)
        fh = new_file(m, "/a")
        used = m._used()
        m.set_faults({"capacity": used + 2 * PAGE}, None)
        n = m.write("/a", b"z" * (3 * PAGE), 0, fh)
        self.assertEqual(n, 2 * PAGE)
        with self.assertRaises(OSError) as e:
            m.write("/a", b"z", 2 * PAGE, fh)
        self.assertEqual(e.exception.errno, errno.ENOSPC)
        # Overwriting allocated space still works under the extent model...
        self.assertEqual(m.write("/a", b"w" * PAGE, 0, fh), PAGE)
        # ...and fails under copy-on-write.
        m.set_faults({"capacity": m._used(), "cow": True}, None)
        with self.assertRaises(OSError):
            m.write("/a", b"w", 0, fh)

    def test_erofs_refuses_mutation_not_reads(self):
        m = ChaosModel(0)
        fh = new_file(m, "/a", b"data")
        m.set_faults({"erofs": True}, None)
        with self.assertRaises(OSError) as e:
            m.write("/a", b"x", 0, fh)
        self.assertEqual(e.exception.errno, errno.EROFS)
        self.assertEqual(m.read("/a", 4, 0, fh), b"data")


class UnlinkTest(unittest.TestCase):
    def test_unlinked_file_readable_through_handle(self):
        m = ChaosModel(0)
        fh = new_file(m, "/a", b"keep")
        m.unlink("/a")
        self.assertEqual(m.read(None, 4, 0, fh), b"keep")
        m.release(fh)
        self.assertEqual(len(m.inodes), 1)  # only the root


if __name__ == "__main__":
    unittest.main()
