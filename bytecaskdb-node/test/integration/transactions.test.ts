// Snapshot and WritePlan (transaction) tests for ByteCaskDB
import { test, expect, decodeBytes } from '../fixtures/index.js'

test('snapshot creates frozen read-only view', async ({ db, wasmBackend }) => {
  // Setup initial data
  const key1 = 'snap-key-1'
  const key2 = 'snap-key-2'
  const value1 = 'snap-value-1'
  const value2 = 'snap-value-2'

  db.put(key1, value1)

  // Create snapshot
  const snapshot = db.snapshot()

  // Modify database after snapshot
  db.put(key2, value2)
  db.del(key1)

  // Snapshot should see original state
  expect(decodeBytes(snapshot.get(key1)!)).toBe(value1)
  expect(snapshot.get(key2)).toBeNull()
  expect(snapshot.containsKey(key1)).toBe(true)
  expect(snapshot.containsKey(key2)).toBe(false)

  // Database should see new state
  expect(db.get(key1)).toBeNull()
  expect(decodeBytes(db.get(key2)!)).toBe(value2)

  snapshot.close()
})

test('WritePlan construction works correctly', async ({ db, wasmBackend }) => {
  // Test snapshot-less WritePlan
  const plan1 = new wasmBackend.WritePlan()
  expect(plan1.hasSnapshot()).toBe(false)

  // Test snapshot-backed WritePlan
  const snapshot = db.snapshot()
  const plan2 = wasmBackend.WritePlan.withSnapshot(snapshot)
  expect(plan2.hasSnapshot()).toBe(true)

  plan1.close()
  plan2.close()
  snapshot.close()
})

test('applyBatch with simple WritePlan works correctly', async ({ db, wasmBackend }) => {
  const plan = new wasmBackend.WritePlan()

  // Add operations to plan
  const key1 = 'batch-key-1'
  const key2 = 'batch-key-2'
  const key3 = 'batch-key-3'
  const value1 = 'batch-value-1'
  const value2 = 'batch-value-2'

  plan.put(key1, value1)
  plan.put(key2, value2)
  plan.del(key3) // Delete non-existent key

  // Apply batch
  const success = db.applyBatch(plan)
  expect(success).toBe(true)

  // Verify results
  expect(decodeBytes(db.get(key1)!)).toBe(value1)
  expect(decodeBytes(db.get(key2)!)).toBe(value2)
  expect(db.get(key3)).toBeNull()
})

test('applyBatch with snapshot detects conflicts', async ({ db, wasmBackend }) => {
  const key = 'conflict-key'
  const originalValue = 'original'
  const conflictValue = 'conflict'
  const planValue = 'plan'

  // Setup initial data
  db.put(key, originalValue)

  // Create snapshot and WritePlan
  const snapshot = db.snapshot()
  const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
  plan.put(key, planValue)

  // Modify database to create conflict
  db.put(key, conflictValue)

  // Apply batch should detect conflict and return false
  const success = db.applyBatch(plan)
  expect(success).toBe(false)

  // Database should still have the conflict value
  expect(decodeBytes(db.get(key)!)).toBe(conflictValue)

  plan.close()
  snapshot.close()
})

test('applyBatch without conflicts succeeds', async ({ db, wasmBackend }) => {
  const key1 = 'no-conflict-1'
  const key2 = 'no-conflict-2'
  const value1 = 'value-1'
  const value2 = 'value-2'
  const newValue = 'new-value'

  // Setup initial data
  db.put(key1, value1)

  // Create snapshot and WritePlan
  const snapshot = db.snapshot()
  const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
  plan.put(key2, newValue) // New key, no conflict
  plan.del(key1) // Delete existing key

  // Apply batch should succeed
  const success = db.applyBatch(plan)
  expect(success).toBe(true)

  // Verify results
  expect(db.get(key1)).toBeNull()
  expect(decodeBytes(db.get(key2)!)).toBe(newValue)

  plan.close()
  snapshot.close()
})

test('ensurePresent guard works correctly', async ({ db, wasmBackend }) => {
  const existingKey = 'existing'
  const missingKey = 'missing'
  const existingValue = 'exists'

  db.put(existingKey, existingValue)

  // Test successful case
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensurePresent(existingKey)
    plan.put('test', 'value')

    const success = db.applyBatch(plan)
    expect(success).toBe(true)

    plan.close()
    snapshot.close()
  }

  // Test failure case
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensurePresent(missingKey) // This should cause failure
    plan.put('test2', 'value2')

    const success = db.applyBatch(plan)
    expect(success).toBe(false)

    plan.close()
    snapshot.close()
  }
})

test('ensureAbsent guard works correctly', async ({ db, wasmBackend }) => {
  const existingKey = 'existing'
  const missingKey = 'missing'
  const existingValue = 'exists'

  db.put(existingKey, existingValue)

  // Test successful case (key is absent)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureAbsent(missingKey)
    plan.put('test', 'value')

    const success = db.applyBatch(plan)
    expect(success).toBe(true)

    plan.close()
    snapshot.close()
  }

  // Test failure case (key exists)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureAbsent(existingKey) // This should cause failure
    plan.put('test2', 'value2')

    const success = db.applyBatch(plan)
    expect(success).toBe(false)

    plan.close()
    snapshot.close()
  }
})

test('ensureUnchanged guard works correctly', async ({ db, wasmBackend }) => {
  const key = 'unchanged-test'
  const originalValue = 'original'
  const changedValue = 'changed'

  db.put(key, originalValue)

  // Test successful case (key unchanged)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureUnchanged(key)
    plan.put('other', 'value')

    const success = db.applyBatch(plan)
    expect(success).toBe(true)

    plan.close()
    snapshot.close()
  }

  // Test failure case (key changed)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureUnchanged(key)
    plan.put('other2', 'value2')

    // Change the key after snapshot
    db.put(key, changedValue)

    const success = db.applyBatch(plan)
    expect(success).toBe(false)

    plan.close()
    snapshot.close()
  }
})

test('ensureRangeUnchanged guard works correctly', async ({ db, wasmBackend }) => {
  const keys = ['range:a', 'range:b', 'range:c']
  const values = ['value-a', 'value-b', 'value-c']

  // Setup initial data
  for (let i = 0; i < keys.length; i++) {
    db.put(keys[i], values[i])
  }

  // Test successful case (range unchanged)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureRangeUnchanged('range:', 'range:~')
    plan.put('other', 'value')

    const success = db.applyBatch(plan)
    expect(success).toBe(true)

    plan.close()
    snapshot.close()
  }

  // Test failure case (range changed)
  {
    const snapshot = db.snapshot()
    const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
    plan.ensureRangeUnchanged('range:', 'range:~')
    plan.put('other2', 'value2')

    // Modify something in the range
    db.put(keys[1], 'modified-value')

    const success = db.applyBatch(plan)
    expect(success).toBe(false)

    plan.close()
    snapshot.close()
  }
})

test('snapshot isolation verification', async ({ db, wasmBackend }) => {
  const key1 = 'isolation-1'
  const key2 = 'isolation-2'
  const value1 = 'value-1'
  const value2 = 'value-2'
  const modified1 = 'modified-1'

  // Setup initial state
  db.put(key1, value1)

  // Create multiple snapshots
  const snapshot1 = db.snapshot()

  // Modify database
  db.put(key1, modified1)
  db.put(key2, value2)

  const snapshot2 = db.snapshot()

  // Verify isolation
  expect(decodeBytes(snapshot1.get(key1)!)).toBe(value1) // Original value
  expect(snapshot1.get(key2)).toBeNull() // Key didn't exist

  expect(decodeBytes(snapshot2.get(key1)!)).toBe(modified1) // Modified value
  expect(decodeBytes(snapshot2.get(key2)!)).toBe(value2) // New key exists

  expect(decodeBytes(db.get(key1)!)).toBe(modified1) // Current state
  expect(decodeBytes(db.get(key2)!)).toBe(value2) // Current state

  snapshot1.close()
  snapshot2.close()
})

test('WritePlan resource cleanup works correctly', async ({ db, wasmBackend }) => {
  const plan = new wasmBackend.WritePlan()

  // Use the plan
  plan.put('cleanup-test', 'cleanup-value')

  // Test explicit close
  expect(() => plan.close()).not.toThrow()

  // Test Symbol.dispose if available
  if (Symbol.dispose in plan) {
    expect(() => plan[Symbol.dispose]()).not.toThrow()
  }
})

test('complex batch operations work correctly', async ({ db, wasmBackend }) => {
  // Setup initial data
  const initialData = [
    { key: 'batch:1', value: 'initial-1' },
    { key: 'batch:2', value: 'initial-2' },
    { key: 'batch:3', value: 'initial-3' },
  ]

  for (const { key, value } of initialData) {
    db.put(key, value)
  }

  const plan = new wasmBackend.WritePlan()

  // Complex batch: put, update, delete, new key
  plan.put('batch:1', 'updated-1') // Update
  plan.del('batch:2') // Delete
  plan.put('batch:4', 'new-4') // New key
  // batch:3 remains unchanged

  const success = db.applyBatch(plan)
  expect(success).toBe(true)

  // Verify results
  expect(decodeBytes(db.get('batch:1')!)).toBe('updated-1')
  expect(db.get('batch:2')).toBeNull()
  expect(decodeBytes(db.get('batch:3')!)).toBe('initial-3')
  expect(decodeBytes(db.get('batch:4')!)).toBe('new-4')
})