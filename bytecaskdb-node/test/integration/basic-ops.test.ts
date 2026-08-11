// Basic CRUD operations tests for ByteCaskDB
import { test, expect, TEST_KEYS, TEST_VALUES, decodeBytes, createTestEntry } from '../fixtures/index.js'

test('put and get round-trip string data correctly', async ({ db }) => {
  const [key, value] = createTestEntry('hello', 'world')

  db.put(key, value)
  const result = db.get(key)

  expect(result).not.toBeNull()
  expect(decodeBytes(result!)).toBe('world')
})

test('put and get round-trip unicode data correctly', async ({ db }) => {
  const key = TEST_KEYS.unicode
  const value = TEST_VALUES.unicode

  db.put(key, value)
  const result = db.get(key)

  expect(result).not.toBeNull()
  expect(decodeBytes(result!)).toBe('测试值-значение-📄')
})

test('get returns null for non-existent keys', async ({ db }) => {
  const result = db.get('non-existent-key')
  expect(result).toBeNull()
})

test('put overwrites existing values', async ({ db }) => {
  const key = 'overwrite-test'
  const value1 = 'first-value'
  const value2 = 'second-value'

  // Put first value
  db.put(key, value1)
  expect(decodeBytes(db.get(key)!)).toBe(value1)

  // Overwrite with second value
  db.put(key, value2)
  expect(decodeBytes(db.get(key)!)).toBe(value2)
  expect(decodeBytes(db.get(key)!)).not.toBe(value1)
})

test('del returns true for existing keys', async ({ db }) => {
  const key = 'delete-existing'
  const value = 'to-be-deleted'

  db.put(key, value)
  expect(decodeBytes(db.get(key)!)).toBe(value)

  const deleted = db.del(key)
  expect(deleted).not.toBeNull()
  expect(db.get(key)).toBeNull()
})

test('del returns false for non-existent keys', async ({ db }) => {
  const key = 'delete-non-existent'

  const deleted = db.del(key)
  expect(deleted).toBeNull()
})

test('containsKey works correctly for existing keys', async ({ db }) => {
  const key = 'contains-existing'
  const value = 'exists'

  expect(db.containsKey(key)).toBe(false)

  db.put(key, value)
  expect(db.containsKey(key)).toBe(true)
})

test('containsKey works correctly for deleted keys', async ({ db }) => {
  const key = 'contains-deleted'
  const value = 'will-be-deleted'

  db.put(key, value)
  expect(db.containsKey(key)).toBe(true)

  db.del(key)
  expect(db.containsKey(key)).toBe(false)
})

test('containsKey works correctly for non-existent keys', async ({ db }) => {
  const key = 'contains-non-existent'
  expect(db.containsKey(key)).toBe(false)
})

test('handles empty keys and values correctly', async ({ db }) => {
  const emptyKey = TEST_KEYS.empty
  const emptyValue = TEST_VALUES.empty
  const regularValue = 'regular-value'
  const regularKey = 'regular-key'

  // Empty key with regular value
  db.put(emptyKey, regularValue)
  expect(decodeBytes(db.get(emptyKey)!)).toBe(regularValue)
  expect(db.containsKey(emptyKey)).toBe(true)

  // Regular key with empty value
  db.put(regularKey, emptyValue)
  expect(decodeBytes(db.get(regularKey)!)).toBe(emptyValue)
  expect(db.containsKey(regularKey)).toBe(true)
})

test('handles large keys and values within limits', async ({ db }) => {
  const largeKey = TEST_KEYS.large // 1000 chars, under 4KB default limit
  const largeValue = TEST_VALUES.large // 10000 chars, under 4MB default limit

  db.put(largeKey, largeValue)
  const result = db.get(largeKey)

  expect(result).not.toBeNull()
  expect(decodeBytes(result!)).toBe(largeValue)
  expect(db.containsKey(largeKey)).toBe(true)
})

test('multiple independent key-value pairs work correctly', async ({ db }) => {
  const pairs = [
    createTestEntry('key1', 'value1'),
    createTestEntry('key2', 'value2'),
    createTestEntry('key3', 'value3'),
  ]

  // Put all pairs
  for (const [key, value] of pairs) {
    db.put(key, value)
  }

  // Verify all pairs
  for (const [key, value] of pairs) {
    expect(decodeBytes(db.get(key)!)).toBe(value)
    expect(db.containsKey(key)).toBe(true)
  }

  // Delete middle pair
  db.del(pairs[1][0])

  // Verify deletion didn't affect others
  expect(decodeBytes(db.get(pairs[0][0])!)).toBe(pairs[0][1])
  expect(db.get(pairs[1][0])).toBeNull()
  expect(decodeBytes(db.get(pairs[2][0])!)).toBe(pairs[2][1])
})

test('binary-like data handling through string encoding', async ({ db }) => {
  // Test that we can store and retrieve binary-like data as strings
  const binaryKey = 'binary-key'
  const binaryValue = '\x00\x01\x02\x03\xFF' // Binary data as string

  db.put(binaryKey, binaryValue)
  const result = db.get(binaryKey)

  expect(result).not.toBeNull()
  expect(decodeBytes(result!)).toBe(binaryValue)
})

test('special character handling', async ({ db }) => {
  const specialChars = [
    { name: 'newlines', key: 'newline-key', value: 'line1\nline2\nline3' },
    { name: 'tabs', key: 'tab-key', value: 'col1\tcol2\tcol3' },
    { name: 'quotes', key: 'quote-key', value: '"single" and \'double\' quotes' },
    { name: 'null', key: 'null-key', value: 'before\0after' },
  ]

  for (const { name, key, value } of specialChars) {
    db.put(key, value)
    const result = db.get(key)
    expect(result, `${name} test failed`).not.toBeNull()
    expect(decodeBytes(result!), `${name} value mismatch`).toBe(value)
  }
})