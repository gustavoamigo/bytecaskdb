#!/usr/bin/env python3
"""Demo of the Pythonic bytecaskdb_ext wrapper covering every feature."""

import tempfile
from bytecaskdb_ext import DB, ConflictError


def main():
    with tempfile.TemporaryDirectory() as tmpdir:

        # ── Open with keyword options ─────────────────────────────────────────
        db = DB.open(tmpdir, max_file_bytes=4 * 1024 * 1024, recovery_threads=2)

        # ── Dict-like writes and reads ────────────────────────────────────────
        print("=== Dict-like interface ===")
        db[b"user:alice"] = b"Alice"
        db[b"user:bob"] = b"Bob"
        db[b"user:carol"] = b"Carol"

        print(f"  db[b'user:alice'] = {db[b'user:alice']}")
        print(f"  b'user:bob' in db = {b'user:bob' in db}")
        print(f"  db.get(b'missing', b'?') = {db.get(b'missing', b'?')}")

        del db[b"user:carol"]
        print(f"  after del, b'user:carol' in db = {b'user:carol' in db}")

        # ── Named writes with options ─────────────────────────────────────────
        print("\n=== put / delete with keyword opts ===")
        db.put(b"fast", b"value", sync=False)
        existed = db.delete(b"fast", sync=False)
        print(f"  delete(fast) existed = {existed}")

        # ── Forward iteration ─────────────────────────────────────────────────
        print("\n=== Forward iteration ===")
        for i in range(1, 6):
            db[f"product:{i}".encode()] = f"item_{i}".encode()

        for key, value in db.items():
            print(f"  {key} -> {value}")

        print("\n  Keys only:")
        for key in db.keys():
            print(f"  {key}")

        # ── Reverse iteration ─────────────────────────────────────────────────
        print("\n=== Reverse iteration (all) ===")
        for key, value in db.ritems():
            print(f"  {key} -> {value}")

        print("\n=== Reverse from b'user:bob' ===")
        for key, value in db.ritems(b"user:bob"):
            print(f"  {key} -> {value}")

        print("\n=== Reverse keys from b'user:bob' ===")
        for key in db.rkeys(b"user:bob"):
            print(f"  {key}")

        # ── Prefix scan ───────────────────────────────────────────────────────
        print("\n=== Prefix scan: product:* ===")
        for key, value in db.prefix(b"product:"):
            print(f"  {key} -> {value}")

        print("\n=== Reverse prefix scan: product:* ===")
        for key, value in db.rprefix(b"product:"):
            print(f"  {key} -> {value}")

        # ── Batch ─────────────────────────────────────────────────────────────
        print("\n=== Batch ===")
        with db.batch(sync=False) as b:
            b[b"product:6"] = b"item_6"
            b[b"product:7"] = b"item_7"
            del b[b"product:1"]
            b.delete_range(b"product:3", b"product:5")  # removes 3 and 4

        print("  After batch:")
        for key, value in db.prefix(b"product:"):
            print(f"  {key} -> {value}")

        # ── Transaction (conflict detection) ──────────────────────────────────
        print("\n=== Transaction: success ===")
        db[b"stock"] = b"10"
        with db.transaction(sync=False) as txn:
            stock = int(txn[b"stock"])
            txn[b"stock"] = str(stock - 1).encode()
        print(f"  stock after txn = {db[b'stock']}")

        print("\n=== Transaction: ConflictError ===")
        snap_before = db.snapshot()
        db[b"stock"] = b"99"  # concurrent write between snap and commit

        try:
            with db.transaction(sync=False) as txn:
                # txn snapshot was taken after the b"99" write, so stock is 99
                # Simulate a concurrent change during the transaction:
                db[b"stock"] = b"0"
                txn[b"stock"] = str(int(txn[b"stock"]) - 1).encode()
        except ConflictError as e:
            print(f"  Caught ConflictError: {e}")
        print(f"  stock unchanged = {db[b'stock']}")

        # ── Transaction retry loop ────────────────────────────────────────────
        print("\n=== Transaction retry loop ===")
        db[b"stock"] = b"5"
        for attempt in range(5):
            try:
                with db.transaction(sync=False) as txn:
                    current = int(txn[b"stock"])
                    txn[b"stock"] = str(current - 1).encode()
                print(f"  Attempt {attempt + 1}: committed, stock -> {current - 1}")
                break
            except ConflictError:
                print(f"  Attempt {attempt + 1}: conflict, retrying...")

        # ── Transaction guards ────────────────────────────────────────────────
        print("\n=== Transaction ensure_absent guard ===")
        try:
            with db.transaction(sync=False) as txn:
                txn.ensure_absent(b"user:alice")  # alice exists → conflict
                txn[b"user:alice"] = b"should not commit"
        except ConflictError as e:
            print(f"  Caught: {e}")

        # ── Snapshot ──────────────────────────────────────────────────────────
        print("\n=== Snapshot ===")
        db[b"snap_key"] = b"before"
        with db.snapshot() as snap:
            db[b"snap_key"] = b"after"
            print(f"  snap[b'snap_key'] = {snap[b'snap_key']}")  # before
            print(f"  db[b'snap_key']   = {db[b'snap_key']}")    # after

            print("  Snapshot ritems from b'user:bob':")
            for key, value in snap.ritems(b"user:bob"):
                print(f"    {key} -> {value}")

            print("  Snapshot rprefix b'user:':")
            for key, value in snap.rprefix(b"user:"):
                print(f"    {key} -> {value}")

        # ── Vacuum ────────────────────────────────────────────────────────────
        print("\n=== Vacuum ===")
        passes = 0
        while db.vacuum(fragmentation_threshold=0.1):
            passes += 1
        print(f"  Vacuum passes: {passes}")

        # ── Degraded state ────────────────────────────────────────────────────
        print("\n=== Degraded state ===")
        print(f"  is_degraded    = {db.is_degraded}")
        print(f"  degraded_reason = '{db.degraded_reason}'")

        print("\nDone.")


if __name__ == "__main__":
    main()
