#!/usr/bin/env python3
"""Iteration patterns: forward, reverse, prefix scan, keys-only."""

import tempfile
import bytecaskdb as bc


def main():
    with tempfile.TemporaryDirectory() as tmpdir:
        db = bc.DB.open(tmpdir)

        # Seed data
        opts = bc.WriteOptions()
        opts.sync = False
        for i in range(1, 6):
            db.put(f"user:{i}".encode(), f"name_{i}".encode(), opts)
        for i in range(1, 4):
            db.put(f"product:{i}".encode(), f"item_{i}".encode(), opts)

        # --- Forward scan (all keys) ---
        print("=== All entries (forward) ===")
        for key, value in db.iter_from():
            print(f"  {key} -> {value}")

        # --- Prefix scan ---
        print("\n=== Prefix scan: user:* ===")
        for key, value in db.iter_from(b"user:"):
            if not key.startswith(b"user:"):
                break
            print(f"  {key} -> {value}")

        # --- Keys-only (no disk I/O) ---
        print("\n=== Keys only ===")
        for key in db.keys_from():
            print(f"  {key}")

        # --- Reverse scan (all keys) ---
        print("\n=== All entries (reverse) ===")
        for key, value in db.riter_from():
            print(f"  {key} -> {value}")

        # --- Reverse keys from a specific point ---
        print("\n=== Reverse keys from user:3 ===")
        for key in db.rkeys_from(b"user:3"):
            print(f"  {key}")

        # --- Snapshot iteration ---
        print("\n=== Snapshot iteration ===")
        db.put(b"user:6", b"name_6", opts)

        with db.snapshot() as snap:
            # snap was taken before user:6 was written... wait, no.
            # Actually snap is taken here, after user:6. Let's show isolation:
            db.put(b"user:7", b"name_7", opts)

            snap_keys = list(snap.keys_from(b"user:"))
            db_keys = list(db.keys_from(b"user:"))

            print(f"  Snapshot sees {len(snap_keys)} user keys (no user:7)")
            print(f"  Live DB sees {len(db_keys)} user keys (includes user:7)")

        print("\nDone.")


if __name__ == "__main__":
    main()
