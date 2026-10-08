// Every error either backend throws carries a `code` (src/types.ts,
// ErrorCode), so a caller can tell the cases apart without matching message
// text. BC_DEGRADED needs a fault the public API cannot cause: see
// test/testing/degraded.test.ts.
import { test, expect } from '../fixtures/index.js'
import { join } from 'node:path'
import { readdir, rm, writeFile, open as openFile } from 'node:fs/promises'
import type { ByteCaskError, ErrorCode } from '../../src/types.js'

function caught(fn: () => unknown): ByteCaskError {
  try {
    fn()
  } catch (e) {
    return e as ByteCaskError
  }
  throw new Error('expected a throw')
}

function expectCode(fn: () => unknown, code: ErrorCode): ByteCaskError {
  const error = caught(fn)
  expect(error).toBeInstanceOf(Error)
  expect(error.code).toBe(code)
  expect(error.message).not.toBe('')
  return error
}

test('a closed DB, snapshot or consumed snapshot reports BC_CLOSED', async ({ tmpDir, wasmBackend }) => {
  const db = wasmBackend.open(join(tmpDir, 'closed'))
  db.put('a', '1')

  const snap = db.snapshot()
  snap.close()
  expectCode(() => snap.get('a'), 'BC_CLOSED')

  const consumed = db.snapshot()
  const plan = wasmBackend.WritePlan.withSnapshot(consumed)
  expectCode(() => consumed.get('a'), 'BC_CLOSED')
  plan.close()
  expectCode(() => plan.put('b', '2'), 'BC_CLOSED')

  const applied = new wasmBackend.WritePlan()
  applied.put('b', '2')
  db.applyBatch(applied)
  expectCode(() => applied.put('c', '3'), 'BC_CLOSED')
  applied.close()

  const manifest = db.createManifest()
  const manifestSnap = manifest.getSnapshot()
  manifest.close()
  manifestSnap.close()
  expectCode(() => manifest.getThroughSequence(), 'BC_CLOSED')
  expectCode(() => manifest.getFiles(), 'BC_CLOSED')
  expectCode(() => manifest.getSnapshot(), 'BC_CLOSED')

  db.close()
  expectCode(() => db.get('a'), 'BC_CLOSED')
  expectCode(() => db.put('b', '2'), 'BC_CLOSED')
  // A second close is not an error.
  db.close()
})

test('a closed iterator is done, not an error', async ({ db }) => {
  db.put('a', '1')
  const it = db.entries('')
  it.close()
  expect(it.next().done).toBe(true)
})

test('a write on a follower reports BC_FOLLOWER_MODE', async ({ tmpDir, wasmBackend }) => {
  const db = wasmBackend.open(join(tmpDir, 'follower'), { initialMode: 'follower' })
  expectCode(() => db.put('a', '1'), 'BC_FOLLOWER_MODE')
  db.close()
})

test('input refused before anything is written reports BC_INVALID_ARGUMENT', async ({ db, wasmBackend }) => {
  expectCode(() => db.delRange('b', 'a'), 'BC_INVALID_ARGUMENT')
  // A JS argument of the wrong type, refused by the binding itself.
  expectCode(() => db.put(1 as unknown as string, 'v'), 'BC_INVALID_ARGUMENT')
  expectCode(() => wasmBackend.open(42 as unknown as string), 'BC_INVALID_ARGUMENT')
  expectCode(() => db.put('k'.repeat(5000), 'v'), 'BC_INVALID_ARGUMENT')
  expectCode(() => wasmBackend.open(join('/nonexistent-never', 'x'), { maxKeyBytes: 70000 }),
    'BC_INVALID_ARGUMENT')
  expect(db.get('b')).toBeNull()
})

test('a call the engine state does not allow reports BC_LOGIC', async ({ db, wasmBackend }) => {
  db.put('a', '1')
  const snap = db.snapshot()
  const entries = [...db.changesSince(snap, 0n)]
  snap.close()
  // ingest is for followers.
  expectCode(() => db.ingest(entries), 'BC_LOGIC')
  // ensureUnchanged needs a plan built from a snapshot.
  const plan = new wasmBackend.WritePlan()
  expectCode(() => plan.ensureUnchanged('a'), 'BC_LOGIC')
  plan.close()
})

test('an I/O failure reports BC_IO with its errno', async ({ tmpDir, wasmBackend }) => {
  // A directory cannot be created under a regular file.
  const file = join(tmpDir, 'a-file')
  await writeFile(file, 'x')
  const error = expectCode(() => wasmBackend.open(join(file, 'db')), 'BC_IO')
  // The native backend reports Linux's numbering, WASM Emscripten's.
  expect(error.errno).toBeGreaterThan(0)
})

test('a corrupt data file reports BC_RUNTIME', async ({ tmpDir, wasmBackend }) => {
  const dir = join(tmpDir, 'corrupt')
  const db = wasmBackend.open(dir, { maxFileBytes: 512 })
  for (let i = 0; i < 100; i++) db.put(`k${String(i).padStart(3, '0')}`, 'v'.repeat(40))
  db.close()

  // Without hints, open rebuilds them from the data files and parses each
  // one. Damage in the newest is a torn tail and is truncated; in any other
  // it is refused. File names do not order the files, so damage them all.
  const files = await readdir(dir)
  for (const f of files.filter((f) => f.endsWith('.hint'))) await rm(join(dir, f))
  const data = files.filter((f) => f.endsWith('.data'))
  expect(data.length).toBeGreaterThan(1)
  for (const f of data) {
    const handle = await openFile(join(dir, f), 'r+')
    await handle.write(Buffer.alloc(64, 0xff), 0, 64, 0)
    await handle.close()
  }

  expectCode(() => wasmBackend.open(dir), 'BC_RUNTIME')
})
