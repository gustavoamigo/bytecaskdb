// Degraded mode through the public API, on the test-only builds
// (bytecask_testing.node, bytecask_testing.mjs; npm run test:testing). Their
// testingFailAt fails I/O at a named engine checkpoint, as the engine's own
// tests do: nothing else in the API can make an fdatasync fail.
import { test as base, expect } from 'vitest'
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { loadNativeModule } from '../../src/native-backend.js'
import { loadWasmModule } from '../../src/wasm-backend.js'
import type { ByteCaskError, ByteCaskFactory } from '../../src/types.js'

interface Testing {
  backend: ByteCaskFactory
  failAt(checkpoint: string): void
  clearFault(): void
}

const test = base.extend<{ testing: Testing; dir: string }>({
  testing: [async ({}, use) => {
    const { backend, module } = process.env.BC_TEST_BACKEND === 'native'
      ? loadNativeModule('bytecask_testing')
      : await loadWasmModule('bytecask_testing')
    await use({
      backend,
      failAt: (checkpoint) => module.testingFailAt(checkpoint),
      clearFault: () => module.testingClearFault(),
    })
  }, { scope: 'worker' }],
  dir: async ({}, use) => {
    const dir = await mkdtemp(join(tmpdir(), 'bytecask-testing-'))
    await use(dir)
    await rm(dir, { recursive: true, force: true })
  },
})

function caught(fn: () => unknown): ByteCaskError {
  try {
    fn()
  } catch (e) {
    return e as ByteCaskError
  }
  throw new Error('expected a throw')
}

test('a failed commit sync degrades the engine until resume()', async ({ testing, dir }) => {
  const db = testing.backend.open(join(dir, 'db'))
  db.put('before', '1')

  // The commit's fdatasync fails: the write throws, and is not visible.
  testing.failAt('io_data_file_sync')
  let error: ByteCaskError
  try {
    error = caught(() => db.put('lost', '2'))
  } finally {
    testing.clearFault()
  }
  expect(error.code).toBe('BC_IO')
  expect(error.errno).toBeGreaterThan(0)
  expect(db.get('lost')).toBeNull()

  expect(db.isDegraded()).toBe(true)
  expect(db.degradedReason()).not.toBe('')

  // Writes are refused; reads still work.
  const refused = caught(() => db.put('after', '3'))
  expect(refused.code).toBe('BC_DEGRADED')
  expect(refused.message).not.toBe('')
  expect(new TextDecoder().decode(db.get('before')!)).toBe('1')

  db.resume()
  expect(db.isDegraded()).toBe(false)
  expect(db.degradedReason()).toBe('')
  // resume() rewrites and syncs the active file, then replays every valid
  // entry in it, so the write that threw can be there after all
  // (CONTRACT.md, classes F and G). Its outcome was unknown, not a failure.
  expect(new TextDecoder().decode(db.get('lost')!)).toBe('2')
  db.put('after', '3')
  db.close()

  const reopened = testing.backend.open(join(dir, 'db'))
  for (const [key, value] of [['before', '1'], ['lost', '2'], ['after', '3']]) {
    expect(new TextDecoder().decode(reopened.get(key)!)).toBe(value)
  }
  reopened.close()
})

test('resume() on an engine that is not degraded does nothing', async ({ testing, dir }) => {
  const db = testing.backend.open(join(dir, 'db'))
  db.put('a', '1')
  db.resume()
  expect(db.isDegraded()).toBe(false)
  expect(new TextDecoder().decode(db.get('a')!)).toBe('1')
  db.close()
})

test('a close whose final sync fails reports BC_IO and still closes', async ({ testing, dir }) => {
  const db = testing.backend.open(join(dir, 'db'))
  db.put('unsynced', '1', { sync: false })

  // close() makes every write durable; its fdatasync fails.
  testing.failAt('io_data_file_sync')
  let error: ByteCaskError
  try {
    error = caught(() => db.close())
  } finally {
    testing.clearFault()
  }
  expect(error.code).toBe('BC_IO')
  expect(error.errno).toBeGreaterThan(0)

  // The handle is released either way.
  expect(caught(() => db.get('unsynced')).code).toBe('BC_CLOSED')
  db.close()

  // And so is the directory lock: the database opens again.
  const reopened = testing.backend.open(join(dir, 'db'))
  reopened.close()
})
