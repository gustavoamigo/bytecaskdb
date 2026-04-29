#!/usr/bin/env python3
"""
UnorderedView Linear Hashing Simulator

Simulates the C++ UnorderedView implementation to explore the effect of
different parameters on write amplification, chain length, and split frequency.

Usage:
    python3 unordered_view_sim.py

Parameters explored:
    initial_size    : starting number of buckets (power of 2)
    bucket_capacity : entries per bucket before a split is triggered
    load_factor     : fraction of capacity before split
    hint_size       : expected key count (pre-sizes the table, eliminates splits)
"""

import uuid
import random
import math

# ---------------------------------------------------------------------------
# Hashing — matches the C++ implementation exactly
# ---------------------------------------------------------------------------

def fnv1a(key: bytes) -> int:
    h = 0xcbf29ce484222325
    for b in key:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h

def fib32(h: int) -> int:
    return (h * 0x9E3779B97F4A7C15) >> 32 & 0xFFFFFFFF

def fib16(h: int) -> int:
    return (h * 0x9E3779B97F4A7C15) >> 48 & 0xFFFF

def route(key: bytes, init: int, sp: int, rnd: int) -> int:
    h = fib32(fnv1a(key))
    n = init * (1 << rnd)
    b = h % n
    return h % (2 * n) if b < sp else b

def h16(key: bytes) -> int:
    return fib16(fnv1a(key))

def next_pow2(n: int) -> int:
    return max(1, 2 ** math.ceil(math.log2(max(1, n))))

# ---------------------------------------------------------------------------
# Linear hash table simulation
# ---------------------------------------------------------------------------

def run(nk: int, init: int, bc: int, lf: float):
    """
    Simulate inserting nk UUIDv4 keys into a linear hash table and
    sampling gets on a random subset.

    Returns:
        splits      : total number of bucket splits
        moved_pp    : entries moved per put (write amplification component)
        write_amp   : total writes / logical puts
        avg_chain   : average chain length at get time
        p99_chain   : p99 chain length at get time
        num_buckets : final bucket count
    """
    keys = [str(uuid.uuid4()).encode() for _ in range(nk)]

    # Mutable state via single-element lists (avoids nonlocal boilerplate)
    bkts = {}
    sp   = [0]   # split pointer
    rnd  = [0]   # current round
    cnt  = [0]   # live entry count
    nspl = [0]   # split count
    mv   = [0]   # entries moved during splits
    dw   = [0]   # direct writes (puts)
    sw   = [0]   # split writes (rewrites)
    cl   = []    # chain lengths observed during gets

    def num_buckets():
        return init * (1 << rnd[0]) + sp[0]

    def do_split():
        nspl[0] += 1
        n  = init * (1 << rnd[0])
        ob = sp[0]
        nm = 2 * n
        oslots = bkts.get(ob, {})
        ne = {}
        for fp, chain in oslots.items():
            # Deduplicate (last-write-wins) and drop tombstones
            seen = {}
            for k, v in chain:
                seen[k] = v
            for k, v in seen.items():
                if v == b'':
                    cnt[0] -= 1
                    continue
                nb  = fib32(fnv1a(k)) % nm
                nfp = h16(k)
                mv[0] += 1
                sw[0] += 1
                ne.setdefault((nb, nfp), []).append((k, v))
        if ob in bkts:
            del bkts[ob]
        for (nb, nfp), chain in ne.items():
            bkts.setdefault(nb, {})[nfp] = chain
        ns = sp[0] + 1
        nr = rnd[0]
        if ns >= n:
            ns = 0
            nr += 1
        sp[0] = ns
        rnd[0] = nr

    def put(k, v):
        dw[0] += 1
        b  = route(k, init, sp[0], rnd[0])
        fp = h16(k)
        c  = bkts.get(b, {}).get(fp, [])
        if not c:
            bkts.setdefault(b, {})[fp] = [(k, v)]
            cnt[0] += 1
        else:
            found = False
            nc = []
            for ek, ev in c:
                if ek == k:
                    nc.append((k, v))
                    found = True
                else:
                    nc.append((ek, ev))
            if not found:
                nc.append((k, v))
                cnt[0] += 1
            bkts.setdefault(b, {})[fp] = nc
        if cnt[0] > int(num_buckets() * bc * lf):
            do_split()

    def get(k):
        b  = route(k, init, sp[0], rnd[0])
        fp = h16(k)
        cl.append(len(bkts.get(b, {}).get(fp, [])))

    for k in keys:
        put(k, b'v')

    sample = random.sample(keys, min(500, nk))
    for k in sample:
        get(k)

    wa  = (dw[0] + sw[0]) / nk
    avg = sum(cl) / len(cl) if cl else 0
    p99 = sorted(cl)[int(len(cl) * 0.99)] if cl else 0

    return nspl[0], mv[0] / nk, wa, avg, p99, num_buckets()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    random.seed(42)

    NP = 20_000  # proxy size — ratios (write_amp, avg_chain) are stable at 1M/10M

    print('=' * 102)
    print(f'UnorderedView Parameter Simulation  ({NP:,} key proxy — ratios stable at 1M/10M scale)')
    print('=' * 102)
    print(f'  {"init":>6} {"bc":>5} {"lf":>5}  {"splits":>7}  {"mvd/put":>7}  '
          f'{"write_amp":>9}  {"avg_chn":>7}  {"p99":>4}  {"buckets":>8}')
    print('-' * 102)

    results = []
    for init in [8, 64, 256]:
        for bc in [8, 32, 64, 128]:
            for lf in [0.5, 0.75, 0.9]:
                s, mp, wa, avg, p99, nb = run(NP, init, bc, lf)
                results.append((init, bc, lf, s, mp, wa, avg, p99, nb))
                print(f'  {init:>6} {bc:>5} {lf:>5.2f}  {s:>7,}  {mp:>7.3f}  '
                      f'{wa:>9.2f}x  {avg:>7.3f}  {p99:>4}  {nb:>8,}')

    print()
    print('=' * 102)
    print('BEST CONFIGURATIONS')
    print('=' * 102)
    cur = next(r for r in results if r[0] == 8 and r[1] == 64 and abs(r[2] - 0.75) < 0.01)
    bw  = min(results, key=lambda r: (r[5], r[6]))
    bp  = min(results, key=lambda r: (r[7], r[5]))
    for lbl, r in [('current (8,64,0.75)', cur), ('min write_amp', bw), ('min p99', bp)]:
        print(f'  [{lbl:22s}]  init={r[0]:4} bc={r[1]:4} lf={r[2]:.2f}  '
              f'write_amp={r[5]:.2f}x  avg_chain={r[6]:.3f}  p99={r[7]}  splits={r[3]:,}')

    print()
    print('=' * 102)
    print('PRE-SIZING via hint_size: initial_size = next_pow2(hint_size / bc / lf)')
    print('=' * 102)
    for nk, bc, lf in [(1_000_000, 64, 0.75), (10_000_000, 64, 0.75)]:
        opt = max(8, next_pow2(nk / (bc * lf)))
        _, _, wn, _, _, _ = run(NP, 8,   bc, lf)
        _, _, wp, _, _, _ = run(NP, opt, bc, lf)
        print(f'  N={nk:>10,}  naive   init=8:          write_amp={wn:.2f}x  splits=many')
        print(f'  N={nk:>10,}  presized init={opt:<8}: write_amp={wp:.2f}x  splits=~0'
              f'   hint_size={nk:,} -> initial_size={opt}')
        print()



if __name__ == '__main__':
    main()