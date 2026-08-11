// Range operations tests for ByteCaskDB
import { test, expect, decodeBytes } from '../fixtures/index.js'

test('delRange single-operation range deletion works correctly', async ({ db }) => {
  // Setup test data with consistent prefix
  const testData = [
    { key: 'range:apple', value: 'fruit-1' },
    { key: 'range:banana', value: 'fruit-2' },
    { key: 'range:cherry', value: 'fruit-3' },
    { key: 'other:item1', value: 'other-1' },
    { key: 'other:item2', value: 'other-2' },
  ]

  // Insert all data
  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Verify all data exists
  for (const { key, value } of testData) {
    expect(decodeBytes(db.get(key)!)).toBe(value)
  }

  // Delete range "range:" to "range:~"
  db.delRange('range:', 'range:~')

  // Verify range keys are deleted
  expect(db.get('range:apple')).toBeNull()
  expect(db.get('range:banana')).toBeNull()
  expect(db.get('range:cherry')).toBeNull()

  // Verify other keys remain
  expect(decodeBytes(db.get('other:item1')!)).toBe('other-1')
  expect(decodeBytes(db.get('other:item2')!)).toBe('other-2')
})

test('WritePlan delRange in batch operations works correctly', async ({ db, wasmBackend }) => {
  // Setup test data
  const rangeData = [
    { key: 'batch:1', value: 'value-1' },
    { key: 'batch:2', value: 'value-2' },
    { key: 'batch:3', value: 'value-3' },
  ]

  const otherData = [
    { key: 'keep:1', value: 'keep-1' },
    { key: 'keep:2', value: 'keep-2' },
  ]

  // Insert all data
  for (const { key, value } of [...rangeData, ...otherData]) {
    db.put(key, value)
  }

  // Create batch with range delete and other operations
  const plan = new wasmBackend.WritePlan()
  plan.delRange('batch:', 'batch:~')
  plan.put('new:key', 'new-value')

  const success = db.applyBatch(plan)
  expect(success).not.toBeNull()

  // Verify range deletion
  for (const { key } of rangeData) {
    expect(db.get(key)).toBeNull()
  }

  // Verify other keys remain
  for (const { key, value } of otherData) {
    expect(decodeBytes(db.get(key)!)).toBe(value)
  }

  // Verify new key was added
  expect(decodeBytes(db.get('new:key')!)).toBe('new-value')
})

test('range iteration with bounds works correctly', async ({ db }) => {
  // Setup ordered test data
  const testData = [
    { key: 'aaa', value: 'value-aaa' },
    { key: 'bbb:1', value: 'value-bbb-1' },
    { key: 'bbb:2', value: 'value-bbb-2' },
    { key: 'bbb:3', value: 'value-bbb-3' },
    { key: 'ccc', value: 'value-ccc' },
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Iterate over range with prefix 'bbb:'
  const entries = db.entries('bbb:')
  const collected = []

  for (const entry of entries) {
    const key = decodeBytes(entry.key)
    if (!key.startsWith('bbb:')) break // Stop when prefix changes
    collected.push({
      key,
      value: decodeBytes(entry.value)
    })
  }

  entries.close()

  // Should only get the bbb: entries in order
  expect(collected).toHaveLength(3)
  expect(collected[0].key).toBe('bbb:1')
  expect(collected[1].key).toBe('bbb:2')
  expect(collected[2].key).toBe('bbb:3')

  for (let i = 0; i < collected.length; i++) {
    expect(collected[i].value).toBe(`value-bbb-${i + 1}`)
  }
})

test('empty range handling works correctly', async ({ db }) => {
  // Setup some test data
  db.put('test:key', 'test:value')

  // Test deleting empty range (from >= to)
  db.delRange('empty:b', 'empty:a') // b >= a, no-op

  // Should not affect existing data
  expect(decodeBytes(db.get('test:key')!)).toBe('test:value')

  // Test deleting range with no matching keys
  db.delRange('nonexistent:', 'nonexistent:~')

  // Should not affect existing data
  expect(decodeBytes(db.get('test:key')!)).toBe('test:value')
})

test('range deletion boundaries work correctly', async ({ db }) => {
  // Setup test data at range boundaries
  const testData = [
    { key: 'prefix', value: 'exact-prefix' }, // Exactly the prefix
    { key: 'prefix:a', value: 'prefix-a' },
    { key: 'prefix:b', value: 'prefix-b' },
    { key: 'prefix:z', value: 'prefix-z' },
    { key: 'prefixa', value: 'after-prefix' }, // Lexicographically after prefix:
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Delete range [prefix:, prefix:~)
  // This should delete prefix:a, prefix:b, prefix:z
  // But NOT 'prefix' (before the range) or 'prefixa' (after the range)
  db.delRange('prefix:', 'prefix:~')

  // Verify boundary behavior
  expect(decodeBytes(db.get('prefix')!)).toBe('exact-prefix') // Before range
  expect(db.get('prefix:a')).toBeNull() // In range
  expect(db.get('prefix:b')).toBeNull() // In range
  expect(db.get('prefix:z')).toBeNull() // In range
  expect(decodeBytes(db.get('prefixa')!)).toBe('after-prefix') // After range
})

test('range operations with unicode keys work correctly', async ({ db }) => {
  // Setup unicode keys with consistent prefix
  const testData = [
    { key: 'unicode:测试1', value: 'unicode-1' },
    { key: 'unicode:测试2', value: 'unicode-2' },
    { key: 'unicode:测试3', value: 'unicode-3' },
    { key: 'other:测试', value: 'different' },
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Delete unicode range - use high Unicode character as upper bound
  db.delRange('unicode:', 'unicodf')

  // Verify deletions
  expect(db.get('unicode:测试1')).toBeNull()
  expect(db.get('unicode:测试2')).toBeNull()
  expect(db.get('unicode:测试3')).toBeNull()

  // Verify key outside range remains
  expect(decodeBytes(db.get('other:测试')!)).toBe('different')
})

test('range deletion with WritePlan conflict detection', async ({ db, wasmBackend }) => {
  const key1 = 'conflict:1'
  const key2 = 'conflict:2'
  const value1 = 'value-1'
  const value2 = 'value-2'
  const newValue = 'new-value'

  // Setup initial data
  db.put(key1, value1)
  db.put(key2, value2)

  // Create snapshot and plan
  const snapshot = db.snapshot()
  const plan = wasmBackend.WritePlan.withSnapshot(snapshot)
  plan.delRange('conflict:', 'conflicu')
  plan.put('new:key', 'new:value')

  // Modify data to create conflict
  db.put(key1, newValue)

  // Apply batch should detect conflict
  const success = db.applyBatch(plan)
  expect(success).toBeNull()

  // Original modification should remain
  expect(decodeBytes(db.get(key1)!)).toBe(newValue)
  expect(decodeBytes(db.get(key2)!)).toBe(value2)

  plan.close()
  snapshot.close()
})

test('range operations handle overlapping ranges correctly', async ({ db }) => {
  // Setup data in overlapping ranges
  const testData = [
    { key: 'a:1', value: 'a1' },
    { key: 'a:2', value: 'a2' },
    { key: 'ab:1', value: 'ab1' },
    { key: 'ab:2', value: 'ab2' },
    { key: 'b:1', value: 'b1' },
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Delete broader range that includes nested ranges
  db.delRange('a', 'b')

  // This should delete everything from 'a' up to but not including 'b'
  expect(db.get('a:1')).toBeNull()
  expect(db.get('a:2')).toBeNull()
  expect(db.get('ab:1')).toBeNull()
  expect(db.get('ab:2')).toBeNull()

  // 'b:1' should remain (not in range [a, b))
  expect(decodeBytes(db.get('b:1')!)).toBe('b1')
})