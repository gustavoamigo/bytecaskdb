#!/usr/bin/env python3
"""Basic ByteCaskDB usage: open, CRUD, batch, close."""

import tempfile
import bytecaskdb as bc


def main():
    with tempfile.TemporaryDirectory() as tmpdir:
        db = bc.DB.open(tmpdir)

        # --- Single-key operations ---
        db.put(b"user:1", b"alice")
        db.put(b"user:2", b"bob")
        db.put(b"user:3", b"carol")

        val = db.get(b"user:1")
        print(f"get(user:1) = {val}")  # b'alice'

        exists = db.contains_key(b"user:2")
        print(f"contains_key(user:2) = {exists}")  # True

        existed = db.del_(b"user:3")
        print(f"del_(user:3) existed = {existed}")  # True

        gone = db.get(b"user:3")
        print(f"get(user:3) after delete = {gone}")  # None

        # --- Batch operations ---
        plan = bc.WritePlan()
        plan.put(b"user:10", b"dave")
        plan.put(b"user:11", b"eve")
        plan.del_(b"user:1")
        db.apply_batch(plan)

        print(f"\nAfter batch:")
        print(f"  get(user:1) = {db.get(b'user:1')}")  # None (deleted)
        print(f"  get(user:10) = {db.get(b'user:10')}")  # b'dave'
        print(f"  get(user:11) = {db.get(b'user:11')}")  # b'eve'

        # --- Options ---
        opts = bc.WriteOptions()
        opts.sync = False  # skip fdatasync for higher throughput
        db.put(b"fast_key", b"fast_value", opts)
        print(f"\nNoSync write: get(fast_key) = {db.get(b'fast_key')}")

        # --- Degraded state check ---
        print(f"\nis_degraded = {db.is_degraded}")  # False
        print(f"degraded_reason = '{db.degraded_reason}'")  # ''

        print("\nDone.")


if __name__ == "__main__":
    main()
