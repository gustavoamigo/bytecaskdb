// Core test fixtures for ByteCaskDB testing
import { test as base, expect } from 'vitest'
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { createWasmBackend } from '../../src/wasm-backend.js'
import type { ByteCaskDB, ByteCaskFactory } from '../../src/index.js'

// Fixture interface definitions
interface WasmFixtures {
  wasmBackend: ByteCaskFactory
}

interface DBFixtures extends WasmFixtures {
  tmpDir: string
  db: ByteCaskDB
}

// Extended test with fixtures
export const test = base.extend<DBFixtures>({
  // WASM backend: expensive initialization, shared across all tests in a worker
  wasmBackend: [async ({}, use) => {
    const backend = await createWasmBackend()
    await use(backend)
    // No explicit cleanup needed for WASM backend
  }, { scope: 'worker' }],

  // Temporary directory: created once per test file, cleaned up after
  tmpDir: [async ({}, use, taskContext) => {
    const testName = taskContext?.name || 'test'
    const dir = await mkdtemp(join(tmpdir(), `bytecask-${testName.replace(/[^a-zA-Z0-9]/g, '-')}-`))
    await use(dir)
    await rm(dir, { recursive: true, force: true })
  }, { scope: 'file' }],

  // Database instance: fresh per test, uses shared WASM backend and file-scoped tmpDir
  db: async ({ tmpDir, wasmBackend }, use) => {
    // Use random ID to create unique database path per test
    const testId = Math.random().toString(36).substring(2, 15)
    const dbPath = join(tmpDir, `test-${testId}.db`)
    const db = wasmBackend.open(dbPath)
    await use(db)
    // Close the database to ensure clean resource release
    await db.close()
  },
})

// Re-export expect for convenience
export { expect }

// Helper functions for common test operations
export function createTestEntry(key: string, value: string): [string, string] {
  return [key, value]
}

// Convert strings to the format expected by the WASM API (no-op since API accepts strings)
export function encodeString(str: string): string {
  return str
}

// Convert Uint8Array results back to strings for testing
export function decodeBytes(bytes: Uint8Array | null): string {
  if (bytes === null) {
    throw new Error('Cannot decode null bytes')
  }
  return new TextDecoder().decode(bytes)
}

// Convert hex string to Uint8Array for binary testing
export function hexToBytes(hexString: string): Uint8Array {
  const cleanHex = hexString.replace(/\s/g, '').toUpperCase()
  return Uint8Array.from(
    cleanHex.match(/.{2}/g)!.map(byte => parseInt(byte, 16))
  )
}

// Common test data - now using strings as per the API
export const TEST_KEYS = {
  simple: 'test-key',
  unicode: '测试-ключ-🔑',
  empty: '',
  large: 'x'.repeat(1000),
}

export const TEST_VALUES = {
  simple: 'test-value',
  unicode: '测试值-значение-📄',
  empty: '',
  large: 'y'.repeat(10000),
}