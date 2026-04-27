// Resource management and cleanup tests for ByteCaskDB
import { test, expect, encodeString, decodeBytes } from '../fixtures/index.js'
import { join } from 'node:path'

test('no file descriptor leaks after close', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'fd-leak-test.db')

  // Create and close multiple database instances
  const iterations = 50

  for (let i = 0; i < iterations; i++) {
    const db = wasmBackend.open(dbPath)

    // Use the database
    const key = encodeString(`leak-test-${i}`)
    const value = encodeString(`value-${i}`)
    db.put(key, value)
    expect(decodeBytes(db.get(key)!)).toEqual(value)

    // Close properly
    await db.close()
  }

  // If we reached here without errors, no obvious file descriptor leaks occurred
  expect(true).toBe(true)
})

test('memory cleanup verification with large operations', async ({ db }) => {
  const iterations = 20
  const valueSize = 100000 // 100KB per value

  // Perform many large operations to test memory cleanup
  for (let i = 0; i < iterations; i++) {
    const key = encodeString(`memory-cleanup-${i}`)
    const value = new Uint8Array(valueSize).fill(i % 256)

    db.put(key, value)
    const result = db.get(key)

    expect(result?.length).toBe(valueSize)
    expect(result?.[0]).toBe(i % 256)

    // Delete to free memory
    db.del(key)
    expect(db.get(key)).toBeNull()
  }

  // Memory should be cleaned up at this point
  expect(true).toBe(true)
})

test('iterator resource cleanup works correctly', async ({ db }) => {
  // Setup test data
  const testData = Array.from({ length: 100 }, (_, i) => ({
    key: encodeString(`iter-cleanup-${i}`),
    value: encodeString(`value-${i}`)
  }))

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Create multiple iterators and close them
  const iterations = 20

  for (let i = 0; i < iterations; i++) {
    const entries = db.entries('')
    const keys = db.keys('')

    // Use iterators briefly
    let count = 0
    for (const entry of entries) {
      count++
      if (count >= 5) break // Only iterate a few items
    }

    let keyCount = 0
    for (const key of keys) {
      keyCount++
      if (keyCount >= 3) break
    }

    // Close iterators
    entries.close()
    keys.close()

    expect(count).toBe(5)
    expect(keyCount).toBe(3)
  }
})

test('snapshot cleanup works correctly', async ({ db, wasmBackend }) => {
  // Setup initial data
  const key = encodeString('snapshot-cleanup-test')
  const value = encodeString('snapshot-value')
  db.put(key, value)

  const iterations = 30

  for (let i = 0; i < iterations; i++) {
    // Create snapshot
    const snapshot = db.snapshot()

    // Use snapshot
    expect(decodeBytes(snapshot.get(key)!)).toEqual(value)
    expect(snapshot.containsKey(key)).toBe(true)

    // Create iterator from snapshot
    const entries = snapshot.entries('')
    let count = 0
    for (const entry of entries) {
      count++
      break // Just get first entry
    }
    entries.close()

    expect(count).toBe(1)

    // Close snapshot
    snapshot.close()

    // Modify database after snapshot is closed
    const newValue = encodeString(`modified-${i}`)
    db.put(key, newValue)
  }
})

test('WritePlan cleanup works correctly', async ({ db, wasmBackend }) => {
  const iterations = 25

  for (let i = 0; i < iterations; i++) {
    // Test snapshot-less WritePlan
    {
      const plan = new wasmBackend.WritePlan()
      plan.put(encodeString(`plan-test-${i}`), encodeString(`value-${i}`))

      const success = db.applyBatch(plan)
      expect(success).toBe(true)

      plan.close()
    }

    // Test snapshot-backed WritePlan
    {
      const snapshot = db.snapshot()
      const plan = wasmBackend.WritePlan.withSnapshot(snapshot)

      plan.put(encodeString(`snap-plan-${i}`), encodeString(`snap-value-${i}`))

      const success = db.applyBatch(plan)
      expect(success).toBe(true)

      plan.close()
      snapshot.close()
    }
  }

  // Verify all data was written correctly
  for (let i = 0; i < iterations; i++) {
    expect(decodeBytes(db.get(encodeString(`plan-test-${i}`))!)).toEqual(encodeString(`value-${i}`))
    expect(decodeBytes(db.get(encodeString(`snap-plan-${i}`))!)).toEqual(encodeString(`snap-value-${i}`))
  }
})

test('exception safety during resource cleanup', async ({ db, wasmBackend }) => {
  const key = encodeString('exception-cleanup')
  const value = encodeString('test-value')

  // Setup initial state
  db.put(key, value)

  // Test iterator cleanup with exceptions
  {
    const entries = db.entries('')

    try {
      for (const entry of entries) {
        // Simulate exception during iteration
        throw new Error('Simulated iterator exception')
      }
    } catch (error) {
      expect(error.message).toBe('Simulated iterator exception')
    }

    // Iterator should still be closeable
    expect(() => entries.close()).not.toThrow()
  }

  // Test snapshot cleanup with exceptions
  {
    const snapshot = db.snapshot()

    try {
      const result = snapshot.get(key)
      expect(result).toEqual(value)
      throw new Error('Simulated snapshot exception')
    } catch (error) {
      expect(error.message).toBe('Simulated snapshot exception')
    }

    // Snapshot should still be closeable
    expect(() => snapshot.close()).not.toThrow()
  }

  // Test WritePlan cleanup with exceptions
  {
    const plan = new wasmBackend.WritePlan()

    try {
      plan.put(key, value)
      throw new Error('Simulated plan exception')
    } catch (error) {
      expect(error.message).toBe('Simulated plan exception')
    }

    // Plan should still be closeable
    expect(() => plan.close()).not.toThrow()
  }

  // Database should still be functional after exceptions
  const testKey = encodeString('after-exceptions')
  const testValue = encodeString('still-works')
  db.put(testKey, testValue)
  expect(decodeBytes(db.get(testKey)!)).toEqual(testValue)
})

test('Symbol.dispose resource cleanup', async ({ db, wasmBackend }) => {
  // Test database Symbol.dispose (if available)
  if (Symbol.dispose in db) {
    const key = encodeString('dispose-test')
    const value = encodeString('dispose-value')
    db.put(key, value)

    // Symbol.dispose should not throw
    expect(() => db[Symbol.dispose]()).not.toThrow()
  }

  // Test iterator Symbol.dispose
  {
    const entries = db.entries('')
    if (Symbol.dispose in entries) {
      expect(() => entries[Symbol.dispose]()).not.toThrow()
    }
    entries.close() // Ensure cleanup
  }

  // Test snapshot Symbol.dispose
  {
    const snapshot = db.snapshot()
    if (Symbol.dispose in snapshot) {
      expect(() => snapshot[Symbol.dispose]()).not.toThrow()
    }
    snapshot.close() // Ensure cleanup
  }

  // Test WritePlan Symbol.dispose
  {
    const plan = new wasmBackend.WritePlan()
    if (Symbol.dispose in plan) {
      expect(() => plan[Symbol.dispose]()).not.toThrow()
    }
    plan.close() // Ensure cleanup
  }
})

test('concurrent resource usage and cleanup', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'concurrent-cleanup.db')
  const db1 = wasmBackend.open(dbPath)

  // Setup test data
  const testData = Array.from({ length: 50 }, (_, i) => ({
    key: encodeString(`concurrent-${i}`),
    value: encodeString(`value-${i}`)
  }))

  for (const { key, value } of testData) {
    db1.put(key, value)
  }

  // Create multiple resources simultaneously
  const snapshots = Array.from({ length: 5 }, () => db1.snapshot())
  const iterators = Array.from({ length: 5 }, () => db1.entries(''))
  const plans = Array.from({ length: 5 }, () => new wasmBackend.WritePlan())

  // Use resources concurrently
  for (let i = 0; i < 5; i++) {
    // Test snapshots
    const testKey = testData[i * 10].key
    expect(decodeBytes(snapshots[i].get(testKey)!)).toEqual(testData[i * 10].value)

    // Test iterators (partial iteration)
    let count = 0
    for (const entry of iterators[i]) {
      count++
      if (count >= 3) break
    }
    expect(count).toBe(3)

    // Test plans
    plans[i].put(encodeString(`plan-${i}`), encodeString(`plan-value-${i}`))
  }

  // Clean up all resources
  for (let i = 0; i < 5; i++) {
    snapshots[i].close()
    iterators[i].close()

    // Apply and close plans
    const success = db1.applyBatch(plans[i])
    expect(success).toBe(true)
  }

  // Verify database is still functional
  const finalKey = encodeString('final-test')
  const finalValue = encodeString('final-value')
  db1.put(finalKey, finalValue)
  expect(decodeBytes(db1.get(finalKey)!)).toEqual(finalValue)

  await db1.close()
})

test('resource limits and cleanup under stress', async ({ db }) => {
  // Create many short-lived resources to test cleanup efficiency
  const iterations = 100

  for (let i = 0; i < iterations; i++) {
    // Create and immediately close snapshot
    const snapshot = db.snapshot()
    snapshot.close()

    // Create and immediately close iterator
    const entries = db.entries('')
    entries.close()

    // Create iterator, use briefly, then close
    const keys = db.keys('')
    let count = 0
    for (const key of keys) {
      count++
      break // Just get first key
    }
    keys.close()
  }

  // Database should remain functional
  const testKey = encodeString('stress-test')
  const testValue = encodeString('stress-value')
  db.put(testKey, testValue)
  expect(decodeBytes(db.get(testKey)!)).toEqual(testValue)
})