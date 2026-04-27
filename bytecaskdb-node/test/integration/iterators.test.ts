// Iterator functionality tests for ByteCaskDB
import { test, expect, decodeBytes } from '../fixtures/index.js'

test('entries() forward iteration works correctly', async ({ db }) => {
  // Setup test data in lexicographic order
  const testData = [
    { key: 'apple', value: 'fruit-1' },
    { key: 'banana', value: 'fruit-2' },
    { key: 'cherry', value: 'fruit-3' },
  ]

  // Insert data
  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Test forward iteration
  const entries = db.entries('')  // Pass empty string instead of undefined
  const collected = []

  for (const entry of entries) {
    collected.push({
      key: decodeBytes(entry.key),
      value: decodeBytes(entry.value)
    })
  }

  entries.close()

  // Should be in lexicographic order
  expect(collected).toHaveLength(3)
  expect(collected[0].key).toBe('apple')
  expect(collected[1].key).toBe('banana')
  expect(collected[2].key).toBe('cherry')

  for (let i = 0; i < testData.length; i++) {
    expect(collected[i].value).toBe(testData[i].value)
  }
})

test('keys() forward iteration works correctly', async ({ db }) => {
  const testKeys = ['key-1', 'key-2', 'key-3']

  // Insert data
  for (const key of testKeys) {
    db.put(key, `value-for-${key}`)
  }

  // Test keys-only iteration
  const keys = db.keys('')
  const collected = []

  for (const key of keys) {
    collected.push(decodeBytes(key))
  }

  keys.close()

  expect(collected).toHaveLength(3)
  expect(collected).toEqual(testKeys.sort()) // Should be sorted
})

test('entriesReverse() backward iteration works correctly', async ({ db }) => {
  const testData = [
    { key: 'alpha', value: 'first' },
    { key: 'beta', value: 'second' },
    { key: 'gamma', value: 'third' },
  ]

  // Insert data
  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Test reverse iteration
  const entries = db.entriesReverse('')
  const collected = []

  for (const entry of entries) {
    collected.push({
      key: decodeBytes(entry.key),
      value: decodeBytes(entry.value)
    })
  }

  entries.close()

  // Should be in reverse lexicographic order
  expect(collected).toHaveLength(3)
  expect(collected[0].key).toBe('gamma')
  expect(collected[1].key).toBe('beta')
  expect(collected[2].key).toBe('alpha')
})

test('keysReverse() backward iteration works correctly', async ({ db }) => {
  const testKeys = ['x', 'y', 'z']

  // Insert data
  for (const key of testKeys) {
    db.put(key, `value-${key}`)
  }

  // Test reverse keys iteration
  const keys = db.keysReverse('')
  const collected = []

  for (const key of keys) {
    collected.push(decodeBytes(key))
  }

  keys.close()

  expect(collected).toHaveLength(3)
  expect(collected).toEqual(['z', 'y', 'x']) // Reverse order
})

test('iterator close() releases resources properly', async ({ db }) => {
  // Setup test data
  db.put('test-key', 'test-value')

  // Create iterator and close it immediately
  const entries = db.entries('')

  // close() should not throw
  expect(() => entries.close()).not.toThrow()

  // Multiple close() calls should be safe
  expect(() => entries.close()).not.toThrow()
})

test('iterator Symbol.dispose works correctly', async ({ db }) => {
  db.put('dispose-key', 'dispose-value')

  const entries = db.entries('')

  // Test Symbol.dispose
  expect(entries[Symbol.dispose]).toBeTypeOf('function')
  expect(() => entries[Symbol.dispose]()).not.toThrow()
})

test('partial iteration and early break work correctly', async ({ db }) => {
  // Insert multiple entries
  const testData = Array.from({ length: 10 }, (_, i) => ({
    key: `key-${i.toString().padStart(2, '0')}`,
    value: `value-${i}`
  }))

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Test breaking early from iteration
  const entries = db.entries('')
  const collected = []

  for (const entry of entries) {
    collected.push(decodeBytes(entry.key))
    if (collected.length >= 3) break // Only collect first 3
  }

  entries.close()

  expect(collected).toHaveLength(3)
  expect(collected[0]).toBe('key-00')
  expect(collected[1]).toBe('key-01')
  expect(collected[2]).toBe('key-02')
})

test('empty database iteration works correctly', async ({ db }) => {
  // Test iterating over empty database
  const entries = db.entries('')
  const collected = []

  for (const entry of entries) {
    collected.push(entry)
  }

  entries.close()

  expect(collected).toHaveLength(0)

  // Same for keys
  const keys = db.keys('')
  const collectedKeys = []

  for (const key of keys) {
    collectedKeys.push(key)
  }

  keys.close()

  expect(collectedKeys).toHaveLength(0)
})

test('iteration with prefix scanning works correctly', async ({ db }) => {
  // Setup data with different prefixes
  const testData = [
    { key: 'user:1', value: 'alice' },
    { key: 'user:2', value: 'bob' },
    { key: 'session:1', value: 'sess1' },
    { key: 'session:2', value: 'sess2' },
    { key: 'config:debug', value: 'true' },
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Test prefix iteration (if supported by the from parameter)
  const userEntries = db.entries('user:')
  const userCollected = []

  for (const entry of userEntries) {
    const key = decodeBytes(entry.key)
    if (!key.startsWith('user:')) break // Stop when prefix changes
    userCollected.push({
      key,
      value: decodeBytes(entry.value)
    })
  }

  userEntries.close()

  expect(userCollected).toHaveLength(2)
  expect(userCollected[0].key).toBe('user:1')
  expect(userCollected[1].key).toBe('user:2')
})

test('concurrent iterator usage works correctly', async ({ db }) => {
  // Setup test data
  const testData = Array.from({ length: 5 }, (_, i) => ({
    key: `concurrent-${i}`,
    value: `value-${i}`
  }))

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Create multiple iterators simultaneously
  const entries1 = db.entries()
  const entries2 = db.entries()
  const keys1 = db.keys()

  const collected1 = []
  const collected2 = []
  const collectedKeys = []

  // Interleave iteration
  const iter1 = entries1[Symbol.iterator]()
  const iter2 = entries2[Symbol.iterator]()
  const iterKeys = keys1[Symbol.iterator]()

  let next1 = iter1.next()
  let next2 = iter2.next()
  let nextKey = iterKeys.next()

  while (!next1.done || !next2.done || !nextKey.done) {
    if (!next1.done) {
      collected1.push(decodeBytes(next1.value.key))
      next1 = iter1.next()
    }
    if (!next2.done) {
      collected2.push(decodeBytes(next2.value.key))
      next2 = iter2.next()
    }
    if (!nextKey.done) {
      collectedKeys.push(decodeBytes(nextKey.value))
      nextKey = iterKeys.next()
    }
  }

  entries1.close()
  entries2.close()
  keys1.close()

  // All iterators should have seen the same data
  expect(collected1).toEqual(collected2)
  expect(collected1).toEqual(collectedKeys)
  expect(collected1).toHaveLength(5)
})

test('iterator resource cleanup on exception', async ({ db }) => {
  // Setup test data
  db.put('exception-test', 'test-value')

  const entries = db.entries('')

  // Simulate an exception during iteration
  try {
    for (const entry of entries) {
      throw new Error('Simulated exception')
    }
  } catch (error) {
    expect(error.message).toBe('Simulated exception')
  }

  // Should still be able to close without issues
  expect(() => entries.close()).not.toThrow()
})

test('for...of loop compatibility', async ({ db }) => {
  const testData = [
    { key: 'loop-1', value: 'value-1' },
    { key: 'loop-2', value: 'value-2' },
  ]

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Test for...of syntax
  const entries = db.entries('')
  const collected = []

  for (const entry of entries) {
    collected.push({
      key: decodeBytes(entry.key),
      value: decodeBytes(entry.value)
    })
  }

  entries.close()

  expect(collected).toHaveLength(2)
  expect(collected[0].key).toBe('loop-1')
  expect(collected[1].key).toBe('loop-2')
})