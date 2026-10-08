// Wrappers left to the garbage collector. V8 runs a wrapper's finalizer
// when it collects it, in an order no test controls, so a snapshot or an
// iterator can be finalized after the DB it came from is closed or
// collected. Under --expose-gc these tests force that; the nightly
// AddressSanitizer run (node-nightly.yml) turns a finalizer that touches a
// freed DB into a crash. vitest.config.ts passes --expose-gc.
import { test, expect, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'
import type { ByteCaskDB, ByteCaskFactory } from '../../src/types.js'

async function collect(): Promise<void> {
  const gc = (globalThis as { gc?: () => void }).gc
  expect(gc, 'run with --expose-gc').toBeDefined()
  // Finalizers run after the collection that finds the object dead, so
  // collect, yield to let them run, and repeat.
  for (let i = 0; i < 3; i++) {
    gc!()
    await new Promise((resolve) => setImmediate(resolve))
  }
}

// Opens one of every wrapper on db and drops the references, unclosed and
// with iterators part-way through.
function abandonWrappers(backend: ByteCaskFactory, db: ByteCaskDB): void {
  for (let i = 0; i < 50; i++) db.put(`k${String(i).padStart(2, '0')}`, `v${i}`)
  const snap = db.snapshot()
  for (const it of [
    db.entries(''), db.keys(''), db.entriesReverse('k~'), db.keysReverse('k~'),
    snap.entries(''), snap.keys(''), db.changesSince(snap, 0n),
  ]) {
    it.next()
  }
  const plan = backend.WritePlan.withSnapshot(db.snapshot())
  plan.put('planned', 'x')
  db.createManifest()
}

test('wrappers finalized after their DB is closed', async ({ tmpDir, wasmBackend }) => {
  const path = join(tmpDir, 'closed')
  const db = wasmBackend.open(path, { maxFileBytes: 1024 })
  abandonWrappers(wasmBackend, db)
  db.close()
  await collect()

  // The directory is released and intact.
  const reopened = wasmBackend.open(path)
  expect(decodeBytes(reopened.get('k07'))).toBe('v7')
  expect(reopened.get('planned')).toBeNull()
  reopened.close()
})

test('wrappers and their DB finalized together, none closed', async ({ tmpDir, wasmBackend }) => {
  const path = join(tmpDir, 'dropped')
  ;(() => {
    const db = wasmBackend.open(path, { maxFileBytes: 1024 })
    abandonWrappers(wasmBackend, db)
  })()
  await collect()

  // Collecting the DB closed it and released its lock.
  const reopened = wasmBackend.open(path)
  expect(decodeBytes(reopened.get('k07'))).toBe('v7')
  reopened.close()
})
