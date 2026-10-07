// Replication: changesSince on a leader, ingest on a follower, and
// createManifest to bootstrap one. Leader and follower are two DBs in one
// process, so each test drives the whole loop a replication service runs.
import { test, expect, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'
import { copyFile, mkdir } from 'node:fs/promises'
import type { ByteCaskDB, ByteCaskFactory, DataEntry } from '../../src/types.js'

function dump(db: ByteCaskDB): Map<string, string> {
  const out = new Map<string, string>()
  const it = db.entries('')
  for (const { key, value } of it) out.set(decodeBytes(key), decodeBytes(value))
  it.close()
  return out
}

// Every durable entry above fromSeq, in the order changesSince yields them.
function stream(leader: ByteCaskDB, fromSeq: bigint): DataEntry[] {
  const snap = leader.snapshot()
  const it = leader.changesSince(snap, fromSeq)
  const entries = [...it]
  it.close()
  snap.close()
  return entries
}

// Cuts the stream into slices of about `size` entries, never inside a
// batch: ingest publishes each slice in one step, so a cut between
// bulkBegin and bulkEnd would expose half a batch.
function slices(entries: DataEntry[], size: number): DataEntry[][] {
  const out: DataEntry[][] = []
  let cur: DataEntry[] = []
  let inBatch = false
  for (const e of entries) {
    cur.push(e)
    if (e.entryType === 'bulkBegin') inBatch = true
    if (e.entryType === 'bulkEnd') inBatch = false
    if (!inBatch && cur.length >= size) {
      out.push(cur)
      cur = []
    }
  }
  if (cur.length > 0) out.push(cur)
  return out
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

test('changesSince streams every entry type in ascending sequence order', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'order')
  writeHistory(wasmBackend, leader, 0)

  const entries = stream(leader, 0n)
  const types = new Set(entries.map((e) => e.entryType))
  expect([...types].sort()).toEqual(['bulkBegin', 'bulkEnd', 'delete', 'put', 'rangeDel'])
  for (let i = 1; i < entries.length; i++) {
    expect(entries[i].sequence > entries[i - 1].sequence).toBe(true)
  }
  expect(entries.at(-1)!.sequence).toBe(leader.durableSequence())

  // From a position, only what follows it.
  const mid = entries[10].sequence
  expect(stream(leader, mid).map((e) => e.sequence)).toEqual(
    entries.filter((e) => e.sequence > mid).map((e) => e.sequence))

  follower.close()
  leader.close()
})

test('a follower that ingests the stream in slices matches the leader', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'slices')
  writeHistory(wasmBackend, leader, 0)
  writeHistory(wasmBackend, leader, 1)

  let last = 0n
  for (const slice of slices(stream(leader, 0n), 7)) {
    follower.ingest(slice)
    // Each slice publishes up to its last entry, and no further.
    expect(follower.durableSequence()).toBe(slice.at(-1)!.sequence)
    expect(follower.durableSequence() > last).toBe(true)
    last = follower.durableSequence()
  }

  expect(dump(leader).size).toBeGreaterThan(20)
  expect(dump(follower)).toEqual(dump(leader))
  expect(follower.durableSequence()).toBe(leader.durableSequence())

  follower.close()
  leader.close()
})

test('a changesSince iterator outlives the snapshot it was taken from', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'outlive')
  writeHistory(wasmBackend, leader, 0)
  const expected = stream(leader, 0n).map((e) => e.sequence)

  const snap = leader.snapshot()
  const it = leader.changesSince(snap, 0n)
  snap.close()
  // Written after the snapshot: not part of this stream.
  leader.put('later', 'x')
  expect([...it].map((e) => e.sequence)).toEqual(expected)
  it.close()

  follower.close()
  leader.close()
})

test('ingest skips entries the follower already holds', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'idem')
  writeHistory(wasmBackend, leader, 0)
  const entries = stream(leader, 0n)

  follower.ingest(entries)
  const once = dump(follower)
  const seq = follower.durableSequence()

  // Delivered again in full, and again overlapping the boundary.
  follower.ingest(entries)
  follower.ingest(entries.slice(-3))
  expect(dump(follower)).toEqual(once)
  expect(follower.durableSequence()).toBe(seq)

  follower.close()
  leader.close()
})

test('a follower restarted mid-stream resumes from its durableSequence', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'restart')
  writeHistory(wasmBackend, leader, 0)

  const first = slices(stream(leader, 0n), 10)
  follower.ingest(first[0])
  follower.ingest(first[1])
  const reached = follower.durableSequence()
  const held = dump(follower)
  follower.close()

  // The leader moves on while the follower is down.
  writeHistory(wasmBackend, leader, 1)

  const reopened = wasmBackend.open(join(tmpDir, 'restart-follower'), { initialMode: 'follower' })
  expect(reopened.durableSequence()).toBe(reached)
  expect(dump(reopened)).toEqual(held)

  for (const slice of slices(stream(leader, reopened.durableSequence()), 10)) reopened.ingest(slice)
  expect(dump(reopened)).toEqual(dump(leader))
  expect(reopened.durableSequence()).toBe(leader.durableSequence())

  reopened.close()
  leader.close()
})

test('ingest is refused on a leader, and writes on a follower', async ({ tmpDir, wasmBackend }) => {
  const { leader, follower } = openPair(wasmBackend, tmpDir, 'modes')
  leader.put('a', '1')
  const entries = stream(leader, 0n)

  expect(() => leader.ingest(entries)).toThrow()
  expect(() => follower.put('b', '2')).toThrow()
  expect(follower.get('b')).toBeNull()

  // Promotion: the follower takes over as leader with the leader's data.
  follower.ingest(entries)
  follower.setMode('leader')
  expect(follower.mode()).toBe('leader')
  const result = follower.put('b', '2')
  expect(result.sequence > entries.at(-1)!.sequence).toBe(true)
  expect(decodeBytes(follower.get('a'))).toBe('1')

  follower.close()
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

  follower.ingest(stream(leader, through))
  expect(dump(follower)).toEqual(dump(leader))
  expect(follower.durableSequence()).toBe(leader.durableSequence())

  follower.close()
  leader.close()
})
