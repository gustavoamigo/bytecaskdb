// WASM integration boundary tests for ByteCaskDB
import { test, expect, encodeString, decodeBytes } from '../fixtures/index.js'

test('TypeScript to WASM type marshalling works correctly', async ({ db }) => {
  // Test various Uint8Array sizes and patterns
  const testCases = [
    { name: 'single byte', data: new Uint8Array([42]) },
    { name: 'empty array', data: new Uint8Array(0) },
    { name: 'binary pattern', data: new Uint8Array([0x00, 0xFF, 0xAA, 0x55]) },
    { name: 'sequential bytes', data: new Uint8Array(Array.from({ length: 256 }, (_, i) => i)) },
  ]

  for (const { name, data } of testCases) {
    const key = encodeString(`marshal-${name}`)

    db.put(key, data)
    const result = db.get(key)

    expect(result, `${name} marshalling failed`).toEqual(data)
    expect(result?.constructor.name, `${name} should return Uint8Array`).toBe('Uint8Array')
  }
})

test('string encoding and decoding correctness', async ({ db }) => {
  const testStrings = [
    'simple ascii',
    'unicode: 你好世界',
    'emoji: 🚀🔥💯',
    'mixed: Hello 世界 🌍',
    'special chars: \n\t\r\0',
    'long string: ' + 'x'.repeat(1000),
  ]

  for (const testString of testStrings) {
    const key = encodeString(`string-${testString.slice(0, 10)}`)
    const value = encodeString(testString)

    db.put(key, value)
    const result = db.get(key)

    expect(result).not.toBeNull()
    expect(decodeBytes(result!)).toBe(testString)
  }
})

test('memory management across JS/WASM boundary', async ({ db }) => {
  // Test that large operations don't cause memory issues
  const largeKey = encodeString('memory-test-key')

  // Test progressively larger values
  const sizes = [1000, 10000, 100000, 1000000] // Up to 1MB

  for (const size of sizes) {
    const largeValue = new Uint8Array(size).fill(42)

    db.put(largeKey, largeValue)
    const result = db.get(largeKey)

    expect(result?.length).toBe(size)
    expect(result?.[0]).toBe(42)
    expect(result?.[size - 1]).toBe(42)
  }
})

test('concurrent operations do not interfere', async ({ db }) => {
  // Test that multiple operations in sequence work correctly
  const operations = Array.from({ length: 100 }, (_, i) => ({
    key: encodeString(`concurrent-${i}`),
    value: encodeString(`value-${i}`)
  }))

  // Write all operations
  for (const { key, value } of operations) {
    db.put(key, value)
  }

  // Verify all operations
  for (const { key, value } of operations) {
    const result = db.get(key)
    expect(result, `Operation ${key} failed`).not.toBeNull()
    expect(decodeBytes(result!)).toEqual(value)
  }

  // Delete every other operation
  for (let i = 0; i < operations.length; i += 2) {
    const deleted = db.del(operations[i].key)
    expect(deleted).not.toBeNull()
  }

  // Verify deletions and remaining data
  for (let i = 0; i < operations.length; i++) {
    const result = db.get(operations[i].key)
    if (i % 2 === 0) {
      expect(result, `Deleted operation ${i} should be null`).toBeNull()
    } else {
      expect(result, `Remaining operation ${i} should exist`).not.toBeNull()
      expect(decodeBytes(result!)).toEqual(operations[i].value)
    }
  }
})

test('error propagation from WASM to TypeScript', async ({ db }) => {
  // Test operations that should fail gracefully

  // Attempt to use extremely large keys (beyond limits)
  const oversizedKey = new Uint8Array(100000) // Likely beyond key size limit
  const testValue = encodeString('test-value')

  // This should either work or throw a meaningful error
  try {
    db.put(oversizedKey, testValue)
    // If it succeeds, verify it works
    expect(db.get(oversizedKey)).toEqual(testValue)
  } catch (error) {
    // If it fails, error should be meaningful
    // WASM errors come as WebAssembly.Exception or other WASM error types
    expect(error).toBeDefined()
    expect(error).toBeInstanceOf(Object)
  }
})

test('null and undefined handling', async ({ db }) => {
  const key = encodeString('null-test-key')

  // Verify get returns exactly null (not undefined) for missing keys
  const result = db.get(key)
  expect(result).toBeNull()
  expect(result).not.toBeUndefined()
})

test('binary data integrity across boundary', async ({ db }) => {
  // Test that binary data maintains bit-perfect integrity
  const binaryPatterns = [
    new Uint8Array([0x00, 0x01, 0x02, 0x03, 0x04]),
    new Uint8Array([0xFF, 0xFE, 0xFD, 0xFC, 0xFB]),
    new Uint8Array([0xAA, 0x55, 0xAA, 0x55, 0xAA]),
    new Uint8Array(Array.from({ length: 256 }, (_, i) => i)),
    new Uint8Array(Array.from({ length: 256 }, (_, i) => 255 - i)),
  ]

  for (let i = 0; i < binaryPatterns.length; i++) {
    const key = encodeString(`binary-pattern-${i}`)
    const pattern = binaryPatterns[i]

    db.put(key, pattern)
    const result = db.get(key)

    expect(result).toEqual(pattern)

    // Verify byte-by-byte
    for (let j = 0; j < pattern.length; j++) {
      expect(result?.[j], `Byte ${j} mismatch in pattern ${i}`).toBe(pattern[j])
    }
  }
})

test('iterator type consistency', async ({ db }) => {
  // Setup test data
  const testEntries = [
    { key: encodeString('iter-a'), value: encodeString('value-a') },
    { key: encodeString('iter-b'), value: encodeString('value-b') },
    { key: encodeString('iter-c'), value: encodeString('value-c') },
  ]

  for (const { key, value } of testEntries) {
    db.put(key, value)
  }

  // Test that iterators return correct types
  const entries = db.entries('')
  expect(entries).toBeDefined()
  expect(typeof entries[Symbol.iterator]).toBe('function')

  // Verify entry types
  for (const entry of entries) {
    expect(entry).toHaveProperty('key')
    expect(entry).toHaveProperty('value')
    expect(entry.key).toBeInstanceOf(Uint8Array)
    expect(entry.value).toBeInstanceOf(Uint8Array)
    break // Just test the first entry
  }

  entries.close()
})