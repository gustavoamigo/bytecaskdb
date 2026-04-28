#!/usr/bin/env python3
"""MVCC transactions with WritePlan: snapshot isolation, conflict detection, retry."""

import tempfile
import bytecaskdb as bc


def main():
    with tempfile.TemporaryDirectory() as tmpdir:
        db = bc.DB.open(tmpdir)

        # Seed some data
        db.put(b"stock:widget", b"100")
        db.put(b"price:widget", b"25")

        # --- Example 1: Decrement stock (write key auto-checked) ---
        print("=== Decrement stock ===")
        snap = db.snapshot()
        stock = int(snap.get(b"stock:widget"))
        new_stock = str(stock - 1).encode()

        plan = bc.WritePlan(snap)
        plan.put(b"stock:widget", new_stock)

        if db.apply_batch(plan):
            print(f"  Stock decremented: {stock} -> {stock - 1}")
        else:
            print("  Conflict! Another writer changed stock.")

        print(f"  Current stock: {db.get(b'stock:widget')}")

        # --- Example 2: Conflict detection ---
        print("\n=== Conflict detection ===")
        snap1 = db.snapshot()
        stock1 = int(snap1.get(b"stock:widget"))

        # Simulate a concurrent writer
        db.put(b"stock:widget", b"50")
        print(f"  Concurrent writer set stock to 50")

        plan1 = bc.WritePlan(snap1)
        plan1.put(b"stock:widget", str(stock1 - 1).encode())

        committed = db.apply_batch(plan1)
        print(f"  Plan committed = {committed}")  # False — conflict
        print(f"  Stock unchanged at: {db.get(b'stock:widget')}")  # b'50'

        # --- Example 3: ensure_unchanged for read dependencies ---
        print("\n=== Order at current price ===")
        snap2 = db.snapshot()
        price = int(snap2.get(b"price:widget"))
        order_total = str(price * 3).encode()  # buy 3 widgets

        plan2 = bc.WritePlan(snap2)
        plan2.ensure_unchanged(b"price:widget")  # guard: reject if price changed
        plan2.put(b"order:001", order_total)

        if db.apply_batch(plan2):
            print(f"  Order placed: 3 x ${price} = ${price * 3}")
        else:
            print("  Price changed — re-read and retry")

        # --- Example 4: Retry loop ---
        print("\n=== Retry loop ===")
        for attempt in range(3):
            snap = db.snapshot()
            current = int(snap.get(b"stock:widget"))
            if current <= 0:
                print("  Out of stock!")
                break

            plan = bc.WritePlan(snap)
            plan.put(b"stock:widget", str(current - 1).encode())

            if db.apply_batch(plan):
                print(f"  Attempt {attempt + 1}: success, stock {current} -> {current - 1}")
                break
            else:
                print(f"  Attempt {attempt + 1}: conflict, retrying...")

        print("\nDone.")


if __name__ == "__main__":
    main()
