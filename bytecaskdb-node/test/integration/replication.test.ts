// Replication: changesSince on a leader, ingest on a follower, and
// createManifest to bootstrap one. Leader and follower are two DBs in one
// process, so each test drives the whole loop a replication service runs.
import { test, expect, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'
import { copyFile, mkdir } from 'node:fs/promises'
import { ORIGIN_MARKER } from '../../src/types.js'
import type {
  ByteCaskDB, ByteCaskError, ByteCaskFactory, ChangeHeader, ChangeMarker, DataEntry,
} from '../../src/types.js'

function dump(db: ByteCaskDB): Map<string, string> {
  const out = new Map<string, string>()
  const it = db.entries('')
  for (const { key, value } of it) out.set(decodeBytes(key), decodeBytes(value))
  it.close()
  return out
}

// One changesSince result read whole: the header and every entry of the
// slice, as a replication service would encode them for one ingest.
function slice(src: ByteCaskDB, fromSeq: bigint, maxBytes?: number): { header: ChangeHeader; entries: DataEntry[] } {
  const snap = src.snapshot()
  const batch = src.changesSince(snap, fromSeq, maxBytes)
  const entries = [...batch.entries]
  batch.entries.close()
  snap.close()
  return { header: batch.header, entries }
}

// Every durable entry above fromSeq, in the order changesSince yields them.
function stream(src: ByteCaskDB, fromSeq: bigint): DataEntry[] {
  return slice(src, fromSeq).entries
}

// Ships src's history above dst's position to dst, one changesSince per
// ingest, until a slice comes back empty. Returns the number of slices
// that carried entries.
function replicate(src: ByteCaskDB, dst: ByteCaskDB, maxBytes?: number): number {
  let rounds = 0
  for (;;) {
    const s = slice(src, dst.durableSequence(), maxBytes)
    dst.ingest(s.header, s.entries)
    if (s.entries.length === 0) return rounds
    rounds++
  }
}

// A slice a service hands on from a node that was never promoted.
function fromOrigin(fromSequence: bigint): ChangeHeader {
  return { marker: ORIGIN_MARKER, fromSequence }
}

function caught(fn: () => unknown): ByteCaskError {
  try {
    fn()
  } catch (e) {
    return e as ByteCaskError
  }
  throw new Error('expected a throw')
}

// The marker a changeMarker entry carries: its own sequence and the id in
// its 8-byte little-endian value.
function markerOf(entry: DataEntry): ChangeMarker {
  expect(entry.entryType).toBe('changeMarker')
  expect(entry.key.length).toBe(0)
  expect(entry.value.length).toBe(8)
  const id = new DataView(entry.value.buffer, entry.value.byteOffset, 8).getBigUint64(0, true)
  return { sinceSequence: entry.sequence, id }
}

// A history with every entry type: puts, overwrites, deletes, a range
// delete, and an atomic batch that mixes them.
function writeHistory(backend: ByteCaskFactory, db: ByteCaskDB, round: number): void {
  for (let i = 0; i < 20; i++) db.put(`k${round}-${String(i).padStart(2, '0')}`, `v${round}-${i}`)
  db.put(`k${round}-03`, `overwritten-${round}`)
  db.del(`k${round}-05`)
  db.delRange(`k${round}-10`, `k${round}-13`)
  const plan = new backend.WritePlan()
  plan.put(`batch${round}-a`, 'a')
  plan.put(`batch${round}-b`, 'b')
  plan.del(`k${round}-15`)
  plan.delRange(`k${round}-17`, `k${round}-19`)
  db.applyBatch(plan)
}

function openPair(backend: ByteCaskFactory, dir: string, name: string) {
  const leader = backend.open(join(dir, `${name}-leader`))
  const follower = backend.open(join(dir, `${name}-follower`), { initialMode: 'follower' })
  return { leader, follower }
}

// Bootstraps a follower directory from src's manifest: ships the listed
// files, as a service would, and returns the sequence the copy reaches.
// The caller keeps vacuum away until the copy ends.
async function bootstrap(src: ByteCaskDB, dir: string): Promise<bigint> {
  const manifest = src.createManifest()
  const through = manifest.getThroughSequence()
  await mkdir(dir)
  for (const f of manifest.getFiles()) {
    await copyFile(f.dataPath, join(dir, f.dataPath.split('/').at(-1)!))
    await copyFile(f.hintPath, join(dir, f.hintPath.split('/').at(-1)!))
  }
  manifest.close()
  return through
}

test('changesSince streams every entry type in ascending sequence order', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'order')
  writeHistory(wasmBackend, leader, 0)

  const { header, entries } = slice(leader, 0n)
  // A leader that was never promoted is on the origin history.
  expect(header).toEqual(fromOrigin(0n))
  const types = new Set(entries.map((e) => e.entryType))
  expect([...types].sort()).toEqual(['bulkBegin', 'bulkEnd', 'delete', 'put', 'rangeDel'])
  for (let i = 1; i < entries.length; i++) {
    expect(entries[i].sequence > entries[i - 1].sequence).toBe(true)
  }
  expect(entries.at(-1)!.sequence).toBe(leader.durableSequence())

  // From a position, only what follows it, and the header names it.
  const mid = entries[10].sequence
  const later = slice(leader, mid)
  expect(later.header).toEqual(fromOrigin(mid))
  expect(later.entries.map((e) => e.sequence)).toEqual(
    entries.filter((e) => e.sequence > mid).map((e) => e.sequence))

  follower.close()
  leader.close()
})

test('a follower that ingests the stream in slices matches the leader', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'slices')
  writeHistory(wasmBackend, leader, 0)
  writeHistory(wasmBackend, leader, 1)
  const total = stream(leader, 0n).length

  let last = 0n
  let rounds = 0
  for (;;) {
    const s = slice(leader, follower.durableSequence(), 200)
    if (s.entries.length === 0) break
    expect(s.header.fromSequence).toBe(follower.durableSequence())
    follower.ingest(s.header, s.entries)
    rounds++
    // Each slice publishes up to its last entry, and no further.
    expect(follower.durableSequence()).toBe(s.entries.at(-1)!.sequence)
    expect(follower.durableSequence() > last).toBe(true)
    last = follower.durableSequence()
  }
  // maxBytes cut the history into several slices, none of them one entry.
  expect(rounds).toBeGreaterThan(1)
  expect(rounds).toBeLessThan(total)

  expect(dump(leader).size).toBeGreaterThan(20)
  expect(dump(follower)).toEqual(dump(leader))
  expect(follower.durableSequence()).toBe(leader.durableSequence())

  follower.close()
  leader.close()
})

test('maxBytes cuts a slice after the unit that passes it, never inside a batch', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'maxbytes')
  writeHistory(wasmBackend, leader, 0)
  const all = stream(leader, 0n)

  // One byte: the first unit alone, a standalone put.
  const first = slice(leader, 0n, 1)
  expect(first.entries).toEqual([all[0]])

  // Cut in front of the atomic batch: the whole batch, bulkBegin to bulkEnd.
  const begin = all.findIndex((e) => e.entryType === 'bulkBegin')
  const end = all.findIndex((e) => e.entryType === 'bulkEnd')
  expect(begin).toBeGreaterThan(0)
  const batch = slice(leader, all[begin - 1].sequence, 1)
  expect(batch.entries.map((e) => e.sequence)).toEqual(all.slice(begin, end + 1).map((e) => e.sequence))

  // The cut slices reassemble the stream, and no slice ends inside a batch.
  const got: DataEntry[] = []
  let pos = 0n
  for (;;) {
    const s = slice(leader, pos, 1)
    if (s.entries.length === 0) break
    expect(s.header).toEqual(fromOrigin(pos))
    let inBatch = false
    for (const e of s.entries) {
      if (e.entryType === 'bulkBegin') inBatch = true
      if (e.entryType === 'bulkEnd') inBatch = false
    }
    expect(inBatch).toBe(false)
    got.push(...s.entries)
    pos = s.entries.at(-1)!.sequence
  }
  expect(got.map((e) => e.sequence)).toEqual(all.map((e) => e.sequence))

  // Omitted: no cut. A follower fed one unit at a time ends up the same.
  expect(slice(leader, 0n).entries.length).toBe(all.length)
  expect(replicate(leader, follower, 1)).toBeGreaterThan(all.length / 2)
  expect(dump(follower)).toEqual(dump(leader))

  follower.close()
  leader.close()
})

test('a changesSince iterator outlives the snapshot it was taken from', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'outlive')
  writeHistory(wasmBackend, leader, 0)
  const expected = stream(leader, 0n).map((e) => e.sequence)

  const snap = leader.snapshot()
  const { entries } = leader.changesSince(snap, 0n)
  snap.close()
  // Written after the snapshot: not part of this stream.
  leader.put('later', 'x')
  expect([...entries].map((e) => e.sequence)).toEqual(expected)
  entries.close()

  follower.close()
  leader.close()
})

test('ingest skips entries the follower already holds', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'idem')
  writeHistory(wasmBackend, leader, 0)
  const whole = slice(leader, 0n)

  follower.ingest(whole.header, whole.entries)
  const once = dump(follower)
  const seq = follower.durableSequence()

  // Delivered again in full, and again overlapping the boundary.
  follower.ingest(whole.header, whole.entries)
  const tail = slice(leader, whole.entries[whole.entries.length - 8].sequence)
  follower.ingest(tail.header, tail.entries)
  // An empty slice at the follower's position is accepted too.
  follower.ingest(fromOrigin(seq), [])
  expect(dump(follower)).toEqual(once)
  expect(follower.durableSequence()).toBe(seq)

  follower.close()
  leader.close()
})

test('a slice that starts past the follower is a gap, refused before anything is written', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'gap')
  writeHistory(wasmBackend, leader, 0)
  const all = stream(leader, 0n)

  // The first five units, all standalone puts.
  follower.ingest(fromOrigin(0n), all.slice(0, 5))
  const reached = follower.durableSequence()
  expect(reached).toBe(all[4].sequence)

  // A service that resumes from the wrong position: the slice starts above
  // the follower, and what lies between would be lost.
  const error = caught(() => follower.ingest(fromOrigin(all[6].sequence), all.slice(7)))
  expect(error.code).toBe('BC_INVALID_ARGUMENT')
  expect(follower.durableSequence()).toBe(reached)
  expect(follower.get(decodeBytes(all[7].key))).toBeNull()

  // From its own position, the follower takes the rest.
  const rest = slice(leader, reached)
  follower.ingest(rest.header, rest.entries)
  expect(dump(follower)).toEqual(dump(leader))

  follower.close()
  leader.close()
})

test('ingest is refused on a leader, and writes on a follower', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'modes')
  leader.put('a', '1')
  const { header, entries } = slice(leader, 0n)

  expect(() => leader.ingest(header, entries)).toThrow()
  expect(() => follower.put('b', '2')).toThrow()
  expect(follower.get('b')).toBeNull()

  // Promotion: the follower takes over as leader with the leader's data.
  // The marker takes the next sequence, the first write the one after.
  follower.ingest(header, entries)
  follower.setMode('leader')
  expect(follower.mode()).toBe('leader')
  const last = entries.at(-1)!.sequence
  expect(follower.durableSequence()).toBe(last + 1n)
  const result = follower.put('b', '2')
  expect(result.sequence).toBe(last + 2n)
  expect(decodeBytes(follower.get('a'))).toBe('1')

  follower.close()
  leader.close()
})

test('a promotion writes a change marker that changesSince streams and names', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'promote')
  writeHistory(wasmBackend, leader, 0)
  replicate(leader, follower)
  const before = follower.durableSequence()

  // Nothing promoted yet: no marker anywhere, and the header reads the origin.
  expect(stream(follower, 0n).some((e) => e.entryType === 'changeMarker')).toBe(false)

  follower.setMode('leader')
  const at = follower.durableSequence()
  expect(at).toBe(before + 1n)

  const whole = slice(follower, 0n)
  expect(whole.header).toEqual(fromOrigin(0n))
  const markers = whole.entries.filter((e) => e.entryType === 'changeMarker')
  expect(markers).toHaveLength(1)
  const marker = markerOf(markers[0])
  expect(marker.sinceSequence).toBe(at)
  expect(marker.id).not.toBe(0n)

  // Below the marker the history is still the origin's; from it on, the
  // marker names the history.
  expect(slice(follower, before).header).toEqual(fromOrigin(before))
  expect(slice(follower, at).header).toEqual({ marker, fromSequence: at })
  follower.put('after', 'x')
  expect(slice(follower, at + 1n).header).toEqual({ marker, fromSequence: at + 1n })

  // Opening in leader mode is not a promotion: the old leader holds none.
  expect(stream(leader, 0n).some((e) => e.entryType === 'changeMarker')).toBe(false)

  follower.close()
  leader.close()
})

test('a follower ahead of a promoted node is a fork, refused with BC_CHANGE_MARKER_MISMATCH', async ({ tmpDir, wasmBackend }) => {
  // Leader L; followers N at 10 and F at 20, each bootstrapped from a manifest.
  const L = wasmBackend.open(join(tmpDir, 'fork-L'))
  for (let i = 1; i <= 10; i++) L.put(`k${String(i).padStart(2, '0')}`, `v${i}`)
  const throughN = await bootstrap(L, join(tmpDir, 'fork-N'))
  for (let i = 11; i <= 20; i++) L.put(`k${String(i).padStart(2, '0')}`, `v${i}`)
  const throughF = await bootstrap(L, join(tmpDir, 'fork-F'))
  const N = wasmBackend.open(join(tmpDir, 'fork-N'), { initialMode: 'follower' })
  const F = wasmBackend.open(join(tmpDir, 'fork-F'), { initialMode: 'follower' })
  expect(N.durableSequence()).toBe(throughN)
  expect(F.durableSequence()).toBe(throughF)
  expect(throughF > throughN).toBe(true)

  // L is lost; N is promoted and writes on.
  N.setMode('leader')
  const marker = markerOf(stream(N, throughN)[0])
  N.put('n', '1')
  const heldByF = dump(F)

  // F holds L's history above N's position: N's marker there is not F's.
  const s = slice(N, F.durableSequence())
  expect(s.header).toEqual({ marker, fromSequence: throughF })
  expect(s.entries).toEqual([])
  const error = caught(() => F.ingest(s.header, s.entries))
  expect(error.code).toBe('BC_CHANGE_MARKER_MISMATCH')
  expect(error.message).not.toBe('')
  expect(F.durableSequence()).toBe(throughF)
  expect(dump(F)).toEqual(heldByF)

  // The old leader, rejoining with writes nobody replicated, the same.
  L.setMode('follower')
  const forL = slice(N, L.durableSequence())
  expect(caught(() => L.ingest(forL.header, forL.entries)).code).toBe('BC_CHANGE_MARKER_MISMATCH')

  // A follower behind N catches up, marker included, and re-delivery of
  // N's history, from before and from after the marker, changes nothing.
  const G = wasmBackend.open(join(tmpDir, 'fork-G'), { initialMode: 'follower' })
  replicate(N, G)
  expect(dump(G)).toEqual(dump(N))
  expect(G.durableSequence()).toBe(N.durableSequence())
  expect(slice(G, G.durableSequence()).header.marker).toEqual(marker)
  const held = dump(G)
  for (const from of [0n, throughN - 2n, marker.sinceSequence, G.durableSequence()]) {
    const again = slice(N, from)
    G.ingest(again.header, again.entries)
  }
  expect(dump(G)).toEqual(held)
  expect(G.durableSequence()).toBe(N.durableSequence())

  // F, bootstrapped again from N, is back in the cluster.
  F.close()
  const dirF2 = join(tmpDir, 'fork-F2')
  await bootstrap(N, dirF2)
  N.put('n', '2')
  const F2 = wasmBackend.open(dirF2, { initialMode: 'follower' })
  replicate(N, F2)
  expect(dump(F2)).toEqual(dump(N))

  F2.close()
  G.close()
  N.close()
  L.close()
})

test('a follower restarted mid-stream resumes from its durableSequence', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'restart')
  writeHistory(wasmBackend, leader, 0)

  const first = slice(leader, 0n, 300)
  follower.ingest(first.header, first.entries)
  const reached = follower.durableSequence()
  expect(reached).toBe(first.entries.at(-1)!.sequence)
  expect(reached < leader.durableSequence()).toBe(true)
  const held = dump(follower)
  follower.close()

  // The leader moves on while the follower is down.
  writeHistory(wasmBackend, leader, 1)

  const reopened = wasmBackend.open(join(tmpDir, 'restart-follower'), { initialMode: 'follower' })
  expect(reopened.durableSequence()).toBe(reached)
  expect(dump(reopened)).toEqual(held)

  replicate(leader, reopened, 300)
  expect(dump(reopened)).toEqual(dump(leader))
  expect(reopened.durableSequence()).toBe(leader.durableSequence())

  reopened.close()
  leader.close()
})

test('createManifest bootstraps a follower that then catches up', async ({ tmpDir, wasmBackend }) => {
  const leader = wasmBackend.open(join(tmpDir, 'manifest-leader'), { maxFileBytes: 1024 })
  writeHistory(wasmBackend, leader, 0)
  writeHistory(wasmBackend, leader, 1)

  const manifest = leader.createManifest()
  const files = manifest.getFiles()
  const through = manifest.getThroughSequence()
  expect(files.length).toBeGreaterThan(1)
  expect(through).toBe(leader.durableSequence())

  const snap = manifest.getSnapshot()
  const atManifest = new Map<string, string>()
  const it = snap.entries('')
  for (const { key, value } of it) atManifest.set(decodeBytes(key), decodeBytes(value))
  it.close()
  snap.close()

  // Ship the listed files. The caller keeps vacuum away until the copy ends.
  const dir = join(tmpDir, 'manifest-follower')
  await mkdir(dir)
  for (const f of files) {
    await copyFile(f.dataPath, join(dir, f.dataPath.split('/').at(-1)!))
    await copyFile(f.hintPath, join(dir, f.hintPath.split('/').at(-1)!))
  }
  manifest.close()

  // The leader keeps writing during the copy.
  writeHistory(wasmBackend, leader, 2)

  const follower = wasmBackend.open(dir, { initialMode: 'follower' })
  expect(dump(follower)).toEqual(atManifest)
  expect(follower.durableSequence()).toBe(through)

  const rest = slice(leader, through)
  expect(rest.header).toEqual(fromOrigin(through))
  follower.ingest(rest.header, rest.entries)
  expect(dump(follower)).toEqual(dump(leader))
  expect(follower.durableSequence()).toBe(leader.durableSequence())

  follower.close()
  leader.close()
})
