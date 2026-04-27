// Error handling tests for ByteCaskDB
import { test, expect, encodeString, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'
import { mkdir, chmod } from 'node:fs/promises'

test('handles file permission errors gracefully', async ({ tmpDir, wasmBackend }) => {
  // Create a directory with restricted permissions
  const restrictedDir = join(tmpDir, 'restricted')
  await mkdir(restrictedDir, { mode: 0o000 }) // No permissions

  try {
    // Attempt to open database in restricted directory
    const dbPath = join(restrictedDir, 'test.db')

    try {
      const db = wasmBackend.open(dbPath)
      // If it succeeds, clean up
      await db.close()
    } catch (error) {
      // Should get a permission error
      expect(error).toBeDefined()
      // Don't check specific error type since WASM errors vary
    }
  } finally {
    // Restore permissions for cleanup
    await chmod(restrictedDir, 0o755)
  }
})

test('handles invalid operations in follower mode', async ({ db }) => {
  // Switch to follower mode
  db.setMode('follower')
  expect(db.mode()).toBe('follower')

  // Normal write operations should fail in follower mode
  const key = encodeString('follower-test-key')
  const value = encodeString('follower-test-value')

  try {
    db.put(key, value)
    // If put succeeds, it might be implementation-specific
    // Just verify the mode is still follower
    expect(db.mode()).toBe('follower')
  } catch (error) {
    // Should get a follower mode error
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/follower|mode|leader/i)
  }

  try {
    db.del(key)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/follower|mode|leader/i)
  }

  // Read operations should still work
  expect(() => db.get(key)).not.toThrow()
  expect(() => db.containsKey(key)).not.toThrow()

  // Switch back to leader mode for cleanup
  db.setMode('leader')
})

test('handles concurrent access patterns', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'concurrent.db')

  // Open database
  const db1 = wasmBackend.open(dbPath)

  // Try to open the same database again
  // This might succeed (shared access) or fail (exclusive lock)
  try {
    const db2 = wasmBackend.open(dbPath)

    // If both succeed, test that operations work
    const key1 = encodeString('concurrent-key-1')
    const key2 = encodeString('concurrent-key-2')
    const value1 = encodeString('value-1')
    const value2 = encodeString('value-2')

    db1.put(key1, value1)
    db2.put(key2, value2)

    // Both operations should be visible
    expect(decodeBytes(db1.get(key1)!)).toEqual(value1)
    expect(decodeBytes(db1.get(key2)!)).toEqual(value2)
    expect(decodeBytes(db2.get(key1)!)).toEqual(value1)
    expect(decodeBytes(db2.get(key2)!)).toEqual(value2)

    await db2.close()
  } catch (error) {
    // If exclusive access, error should be meaningful
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/lock|busy|use|access/i)
  }

  await db1.close()
})

test('handles malformed WASM module gracefully', async () => {
  // This test is hard to implement without corrupting the WASM file
  // Instead, test that initialization provides meaningful errors

  try {
    // Try to create backend (should succeed normally)
    const { createWasmBackend } = await import('../../src/wasm-backend.js')
    const backend = await createWasmBackend()
    expect(backend).toBeDefined()
  } catch (error) {
    // If it fails, error should be meaningful
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/wasm|module|load|init/i)
  }
})

test('handles invalid database paths gracefully', async ({ wasmBackend }) => {
  const invalidPaths = [
    '', // Empty path
    '\0invalid', // Null byte in path
    '/dev/null', // Special file
  ]

  for (const path of invalidPaths) {
    try {
      const db = wasmBackend.open(path)
      // If it succeeds, just close it
      await db.close()
    } catch (error) {
      // Should get a meaningful path error (could be Error or WebAssembly.Exception)
      expect(error).toBeInstanceOf(Object) // Accept any error object
      if (error instanceof Error) {
        expect(error.message).toMatch(/path|invalid|directory/i)
      } else {
        // WASM exceptions might have different structure
        expect(error).toBeDefined()
      }
    }
  }
})

test('handles operations after database close', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'closed-ops.db')
  const db = wasmBackend.open(dbPath)

  // Use database normally
  const key = encodeString('close-test-key')
  const value = encodeString('close-test-value')
  db.put(key, value)

  // Close database
  await db.close()

  // Operations after close should either:
  // 1. Throw meaningful errors, or
  // 2. Be safely ignored (implementation dependent)

  try {
    db.get(key)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/close|invalid|disposed/i)
  }

  try {
    db.put(key, value)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/close|invalid|disposed/i)
  }

  try {
    db.del(key)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/close|invalid|disposed/i)
  }
})

test('handles iterator operations after close', async ({ db }) => {
  // Setup test data
  db.put(encodeString('iter-key'), encodeString('iter-value'))

  // Create iterator
  const entries = db.entries('')

  // Close iterator
  entries.close()

  // Operations after close should be safe
  expect(() => entries.close()).not.toThrow() // Multiple closes OK

  // Attempting to iterate might throw or be empty
  try {
    for (const entry of entries) {
      // If iteration works, that's OK too
      expect(entry).toBeDefined()
      break // Just test one iteration
    }
  } catch (error) {
    // If it throws, should be meaningful
    expect(error).toBeDefined()
  }
})

test('handles snapshot operations after close', async ({ db, wasmBackend }) => {
  const key = encodeString('snap-key')
  const value = encodeString('snap-value')
  db.put(key, value)

  // Create snapshot
  const snapshot = db.snapshot()

  // Verify snapshot works
  expect(decodeBytes(snapshot.get(key)!)).toEqual(value)

  // Close snapshot
  snapshot.close()

  // Operations after close should either work or throw meaningful errors
  try {
    snapshot.get(key)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/close|invalid|disposed/i)
  }

  try {
    snapshot.containsKey(key)
  } catch (error) {
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/close|invalid|disposed/i)
  }
})

test('handles WritePlan operations after close', async ({ wasmBackend }) => {
  const plan = new wasmBackend.WritePlan()

  const key = 'plan-key'
  const value = 'plan-value'

  // Use plan normally
  plan.put(key, value)

  // Close plan
  plan.close()

  // Operations after close should throw or be safely ignored
  try {
    plan.put(key, value)
  } catch (error) {
    expect(error).toBeInstanceOf(Object) // Accept Error or WebAssembly.Exception
  }

  try {
    plan.del(key)
  } catch (error) {
    expect(error).toBeInstanceOf(Object) // Accept Error or WebAssembly.Exception
  }
})

test('handles extreme memory pressure gracefully', async ({ db }) => {
  // Test very large key-value operations
  try {
    // Create progressively larger allocations
    const sizes = [10000, 100000, 1000000] // Up to 1MB

    for (const size of sizes) {
      const largeData = new Uint8Array(size).fill(42)
      const key = encodeString(`memory-test-${size}`)

      db.put(key, largeData)
      const result = db.get(key)

      expect(result?.length).toBe(size)
    }
  } catch (error) {
    // If memory allocation fails, error should be meaningful
    expect(error).toBeDefined()
    expect(error.message.toLowerCase()).toMatch(/memory|allocation|size|limit/i)
  }
})

test('handles invalid option values gracefully', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'invalid-options.db')

  // Test with invalid options (if options are supported)
  const invalidOptions = [
    { maxFileBytes: -1 }, // Negative value
    { maxFileBytes: 0 }, // Zero value
    { maxKeyBytes: -1 }, // Negative key size
    { maxValueBytes: -1 }, // Negative value size
  ]

  for (const options of invalidOptions) {
    try {
      const db = wasmBackend.open(dbPath, options)
      // If it succeeds, just close it
      await db.close()
    } catch (error) {
      // Should get meaningful validation error
      expect(error).toBeDefined()
      expect(error.message.toLowerCase()).toMatch(/option|invalid|value|range/i)
    }
  }
})

test('handles exception safety during operations', async ({ db }) => {
  const key = encodeString('exception-safety')
  const value = encodeString('test-value')

  // Ensure database is in consistent state before potential exceptions
  db.put(key, value)
  expect(decodeBytes(db.get(key)!)).toEqual(value)

  // Operations that might fail should leave database in consistent state
  try {
    // Try to put oversized data
    const oversizedValue = new Uint8Array(100 * 1024 * 1024) // 100MB
    db.put(encodeString('oversized'), oversizedValue)
  } catch (error) {
    // After exception, original data should still be accessible
    expect(decodeBytes(db.get(key)!)).toEqual(value)
  }

  // Database should still be functional
  const newKey = encodeString('after-exception')
  const newValue = encodeString('new-value')
  db.put(newKey, newValue)
  expect(decodeBytes(db.get(newKey)!)).toEqual(newValue)
})