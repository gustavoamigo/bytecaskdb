// Native-only recovery tests for ByteCaskDB (BC-236).
//
// These exercise code paths the shared WASM/native integration suite can't:
// the native backend links the real multi-threaded engine, so closing and
// reopening a database exercises parallel recovery (Options.recovery_threads
// > 1, hard-coded to 4 in native-backend.ts's Options mapping) and the
// background hint-file writer that runs after file rotation. The WASM
// backend is single-threaded (BYTECASK_SINGLE_THREADED, recovery_threads=1)
// and has no comparable code path to exercise, so this file is skipped
// unless BC_TEST_BACKEND=native.
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, test, expect } from 'vitest'
import { createNativeBackend } from '../../src/native-backend.js'
import { encodeString, decodeBytes } from '../fixtures/index.js'

const isNative = process.env.BC_TEST_BACKEND === 'native'

describe.skipIf(!isNative)('native backend recovery (BC-236)', () => {
  test('reopen after close recovers all keys via parallel recovery', async () => {
    const backend = await createNativeBackend()
    const dir = await mkdtemp(join(tmpdir(), 'bc-native-recovery-'))
    try {
      const dbPath = join(dir, 'recovery.db')
      const entries: [string, string][] = Array.from({ length: 500 }, (_, i) => [
        `key-${i.toString().padStart(4, '0')}`,
        `value-${i}`,
      ])

      const db = backend.open(dbPath)
      for (const [k, v] of entries) {
        db.put(encodeString(k), encodeString(v))
      }
      db.close()

      // Reopening triggers hint generation for the sealed active file (if
      // missing) followed by parallel hint replay across recovery_threads.
      const reopened = backend.open(dbPath)
      for (const [k, v] of entries) {
        const got = reopened.get(encodeString(k))
        expect(decodeBytes(got!)).toBe(v)
      }
      reopened.close()
    } finally {
      await rm(dir, { recursive: true, force: true })
    }
  })

  test('reopen after rotation recovers keys written to sealed files', async () => {
    const backend = await createNativeBackend()
    const dir = await mkdtemp(join(tmpdir(), 'bc-native-rotation-'))
    try {
      const dbPath = join(dir, 'rotation.db')
      // A small max_file_bytes forces multiple rotations for a modest number
      // of writes, so recovery must merge several sealed hint files rather
      // than replaying a single active file.
      const db = backend.open(dbPath, { maxFileBytes: 4096 })

      const entries: [string, string][] = Array.from({ length: 200 }, (_, i) => [
        `rot-key-${i.toString().padStart(4, '0')}`,
        `rot-value-${'x'.repeat(32)}-${i}`,
      ])
      for (const [k, v] of entries) {
        db.put(encodeString(k), encodeString(v))
      }
      db.close()

      const reopened = backend.open(dbPath, { maxFileBytes: 4096 })
      for (const [k, v] of entries) {
        const got = reopened.get(encodeString(k))
        expect(decodeBytes(got!)).toBe(v)
      }
      reopened.close()
    } finally {
      await rm(dir, { recursive: true, force: true })
    }
  })
})
