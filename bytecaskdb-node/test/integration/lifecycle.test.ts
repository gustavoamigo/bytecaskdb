// Database lifecycle management tests for ByteCaskDB
import { test, expect, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'
import { createWasmBackend } from '../../src/wasm-backend.js'

test('createWasmBackend initializes successfully', async () => {
  const backend = await createWasmBackend()

  expect(backend).toBeDefined()
  expect(backend.open).toBeTypeOf('function')
  expect(backend.WritePlan).toBeDefined()
})

test('ByteCaskFactory.open creates working DB instances', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'lifecycle-test.db')
  const db = wasmBackend.open(dbPath)

  expect(db).toBeDefined()
  expect(db.put).toBeTypeOf('function')
  expect(db.get).toBeTypeOf('function')
  expect(db.del).toBeTypeOf('function')
  expect(db.close).toBeTypeOf('function')

  // Verify the database is functional
  const key = 'test-key'
  const value = 'test-value'

  db.put(key, value)
  expect(decodeBytes(db.get(key)!)).toBe(value)

  db.close()
})

test('close releases resources properly', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'close-test.db')
  const db = wasmBackend.open(dbPath)

  // Use the database
  const key = 'close-test-key'
  const value = 'close-test-value'
  db.put(key, value)

  // Close should not throw
  expect(() => db.close()).not.toThrow()
})

test('Symbol.dispose works with using statements', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'dispose-test.db')

  // Test using syntax (if supported by the Node.js version)
  try {
    await using db = wasmBackend.open(dbPath)

    const key = 'dispose-test-key'
    const value = 'dispose-test-value'
    db.put(key, value)
    expect(decodeBytes(db.get(key)!)).toBe(value)

    // db should be automatically disposed when leaving this scope
  } catch (error) {
    // If using statements aren't supported, test Symbol.dispose manually
    const db = wasmBackend.open(dbPath)

    const key = 'dispose-test-key'
    const value = 'dispose-test-value'
    db.put(key, value)
    expect(decodeBytes(db.get(key)!)).toBe(value)

    // Manual disposal
    expect(db[Symbol.dispose]).toBeTypeOf('function')
    db[Symbol.dispose]()
  }
})

test('multiple DB instances can coexist', async ({ tmpDir, wasmBackend }) => {
  const dbPath1 = join(tmpDir, 'multi-1.db')
  const dbPath2 = join(tmpDir, 'multi-2.db')

  const db1 = wasmBackend.open(dbPath1)
  const db2 = wasmBackend.open(dbPath2)

  // Write to both databases
  const key1 = 'db1-key'
  const key2 = 'db2-key'
  const value1 = 'db1-value'
  const value2 = 'db2-value'

  db1.put(key1, value1)
  db2.put(key2, value2)

  // Verify isolation
  expect(decodeBytes(db1.get(key1)!)).toBe(value1)
  expect(db1.get(key2)).toBeNull() // key2 should not exist in db1

  expect(decodeBytes(db2.get(key2)!)).toBe(value2)
  expect(db2.get(key1)).toBeNull() // key1 should not exist in db2

  db1.close()
  db2.close()
})

test('opening non-existent directory creates it', async ({ tmpDir, wasmBackend }) => {
  const nonExistentPath = join(tmpDir, 'nested', 'deep', 'path', 'db')

  // This should create the directory structure
  const db = wasmBackend.open(nonExistentPath)

  // Verify the database works
  const key = 'nested-key'
  const value = 'nested-value'
  db.put(key, value)
  expect(decodeBytes(db.get(key)!)).toBe(value)

  db.close()
})

test('reopening same database path preserves data', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'reopen-test.db')
  const key = 'persistent-key'
  const value = 'persistent-value'

  // First session: write data and close
  {
    const db = wasmBackend.open(dbPath)
    db.put(key, value)
    db.close()
  }

  // Second session: reopen and verify data persists
  {
    const db = wasmBackend.open(dbPath)
    expect(decodeBytes(db.get(key)!)).toBe(value)
    expect(db.containsKey(key)).toBe(true)
    db.close()
  }
})

test('database stats are accessible after creation', async ({ db }) => {
  const stats = db.stats()

  expect(stats).toBeTypeOf('object')
  expect(Object.keys(stats).length).toBeGreaterThan(0)

  // All stat values should be numbers
  for (const [key, value] of Object.entries(stats)) {
    expect(value, `stat ${key} should be a number`).toBeTypeOf('number')
  }
})

test('mode operations work correctly', async ({ db }) => {
  // Default mode should be leader
  expect(db.mode()).toBe('leader')

  // Switch to follower mode
  db.setMode('follower')
  expect(db.mode()).toBe('follower')

  // Switch back to leader mode
  db.setMode('leader')
  expect(db.mode()).toBe('leader')
})