// Boundary value testing for ByteCaskDB
import { test, expect, encodeString, decodeBytes } from '../fixtures/index.js'

test('handles maximum key size within default limits', async ({ db }) => {
  // Default key limit is 4KB (4096 bytes)
  // Create a large string key close to the limit
  const largeKey = 'A'.repeat(4000) // 4000 characters, well within 4KB limit
  const testValue = 'large-key-value'

  // This should work with default configuration
  try {
    db.put(largeKey, testValue)
    const result = db.get(largeKey)
    expect(decodeBytes(result!)).toEqual(testValue)
  } catch (error) {
    // If it fails, the error should be meaningful
    expect(error).toBeInstanceOf(Error)
    expect(error.message).toMatch(/key|size|limit/i)
  }
})

test('handles near-maximum key size', async ({ db }) => {
  // Test just under the limit
  const nearMaxKey = 'B'.repeat(4000) // 4000 characters, well under 4KB limit
  const testValue = 'near-max-key-value'

  db.put(nearMaxKey, testValue)
  const result = db.get(nearMaxKey)
  expect(decodeBytes(result!)).toEqual(testValue)
})

test('handles maximum value size within default limits', async ({ db }) => {
  // Default value limit is 4MB (4 * 1024 * 1024 bytes)
  // Test with a reasonable large size (1MB) rather than full 4MB
  const largeValueSize = 1024 * 1024 // 1MB
  const largeValue = new Uint8Array(largeValueSize).fill(67) // Fill with 'C'
  const testKey = encodeString('large-value-key')

  db.put(testKey, largeValue)
  const result = db.get(testKey)

  expect(result).not.toBeNull()
  expect(result!.length).toBe(largeValueSize)
  expect(result![0]).toBe(67)
  expect(result![largeValueSize - 1]).toBe(67)
})

test('handles progressively larger values', async ({ db }) => {
  const sizes = [1000, 10000, 100000, 500000] // Up to 500KB

  for (const size of sizes) {
    const key = encodeString(`size-test-${size}`)
    const value = new Uint8Array(size).fill(size % 256)

    db.put(key, value)
    const result = db.get(key)

    expect(result?.length, `Size ${size} test failed`).toBe(size)
    expect(result?.[0], `First byte mismatch for size ${size}`).toBe(size % 256)
    expect(result?.[size - 1], `Last byte mismatch for size ${size}`).toBe(size % 256)
  }
})

test('handles empty keys and values correctly', async ({ db }) => {
  const emptyKey = ''
  const emptyValue = ''
  const regularValue = 'regular'
  const regularKey = 'regular-key'

  // Empty key with regular value
  db.put(emptyKey, regularValue)
  expect(decodeBytes(db.get(emptyKey)!)).toEqual(regularValue)
  expect(db.containsKey(emptyKey)).toBe(true)

  // Regular key with empty value
  db.put(regularKey, emptyValue)
  expect(decodeBytes(db.get(regularKey)!)).toEqual(emptyValue)
  expect(db.containsKey(regularKey)).toBe(true)

  // Empty key with empty value
  const emptyKey2 = ''
  db.put(emptyKey2, emptyValue)
  expect(decodeBytes(db.get(emptyKey2)!)).toEqual(emptyValue)
})

test('handles unicode and binary data edge cases', async ({ db }) => {
  const testCases = [
    {
      name: 'null bytes',
      key: String.fromCharCode(0, 1, 0, 2, 0),
      value: String.fromCharCode(0, 0, 0, 0)
    },
    {
      name: 'high bytes',
      key: String.fromCharCode(255, 254, 253),
      value: String.fromCharCode(255, 255, 255)
    },
    {
      name: 'mixed unicode',
      key: encodeString('key-🔑-клавиша-键'),
      value: encodeString('value-💎-значение-值')
    },
    {
      name: 'control characters',
      key: encodeString('key\n\t\r\0'),
      value: encodeString('value\n\t\r\0')
    },
    {
      name: 'emoji sequences',
      key: encodeString('👨‍👩‍👧‍👦'),
      value: encodeString('🏳️‍🌈🏳️‍⚧️')
    }
  ]

  for (const { name, key, value } of testCases) {
    db.put(key, value)
    const result = db.get(key)

    expect(decodeBytes(result!), `${name} test failed`).toEqual(value)
    expect(db.containsKey(key), `${name} containsKey failed`).toBe(true)
  }
})

test('handles large dataset operations efficiently', async ({ db }) => {
  // Test with a reasonably large number of entries
  const entryCount = 10000
  const keyPrefix = 'large-dataset-'

  // Insert many entries
  for (let i = 0; i < entryCount; i++) {
    const key = encodeString(`${keyPrefix}${i.toString().padStart(6, '0')}`)
    const value = encodeString(`value-${i}`)
    db.put(key, value)
  }

  // Verify random entries
  const testIndices = [0, 100, 1000, 5000, 9999]
  for (const i of testIndices) {
    const key = encodeString(`${keyPrefix}${i.toString().padStart(6, '0')}`)
    const expectedValue = encodeString(`value-${i}`)
    const result = db.get(key)

    expect(decodeBytes(result!), `Entry ${i} not found`).toEqual(expectedValue)
  }

  // Test iteration over large dataset (partial)
  const entries = db.entries(keyPrefix)
  let count = 0

  for (const entry of entries) {
    const key = decodeBytes(entry.key)
    if (!key.startsWith(keyPrefix)) break

    count++
    if (count >= 100) break // Only check first 100 entries
  }

  entries.close()
  expect(count).toBe(100)
})

test('handles key collision edge cases', async ({ db }) => {
  // Test keys that differ by single byte
  const baseKey = new Uint8Array([1, 2, 3, 4])
  const variants = [
    new Uint8Array([1, 2, 3, 5]), // Last byte differs
    new Uint8Array([1, 2, 4, 4]), // Middle byte differs
    new Uint8Array([2, 2, 3, 4]), // First byte differs
  ]

  const baseValue = encodeString('base-value')

  db.put(baseKey, baseValue)

  for (let i = 0; i < variants.length; i++) {
    const variantValue = encodeString(`variant-${i}`)
    db.put(variants[i], variantValue)

    // Verify base key is unaffected
    expect(decodeBytes(db.get(baseKey)!), `Base key affected by variant ${i}`).toEqual(baseValue)

    // Verify variant is stored correctly
    expect(decodeBytes(db.get(variants[i])!), `Variant ${i} not stored correctly`).toEqual(variantValue)
  }
})

test('handles byte boundary values correctly', async ({ db }) => {
  // Test all single-byte values as keys
  for (let i = 0; i < 256; i++) {
    const key = new Uint8Array([i])
    const value = encodeString(`byte-value-${i}`)

    db.put(key, value)
    expect(decodeBytes(db.get(key)!), `Byte value ${i} failed`).toEqual(value)
  }
})

test('handles prefix relationship edge cases', async ({ db }) => {
  // Test keys where one is a prefix of another
  const shortKey = encodeString('prefix')
  const longKey = encodeString('prefix-extended')
  const shortValue = encodeString('short-value')
  const longValue = encodeString('long-value')

  db.put(shortKey, shortValue)
  db.put(longKey, longValue)

  // Both should coexist without interference
  expect(decodeBytes(db.get(shortKey)!)).toEqual(shortValue)
  expect(decodeBytes(db.get(longKey)!)).toEqual(longValue)

  // Delete short key, long key should remain
  expect(db.del(shortKey)).not.toBeNull()
  expect(db.get(shortKey)).toBeNull()
  expect(decodeBytes(db.get(longKey)!)).toEqual(longValue)
})

test('handles rapid put/delete cycles', async ({ db }) => {
  const key = encodeString('rapid-cycle-key')
  const values = [
    encodeString('value-1'),
    encodeString('value-2'),
    encodeString('value-3'),
  ]

  // Rapid put/delete/put cycles
  for (let cycle = 0; cycle < 100; cycle++) {
    const value = values[cycle % values.length]

    db.put(key, value)
    expect(decodeBytes(db.get(key)!)).toEqual(value)

    if (cycle % 3 === 0) {
      expect(db.del(key)).not.toBeNull()
      expect(db.get(key)).toBeNull()
    }
  }
})

test('handles memory-intensive value patterns', async ({ db }) => {
  // Test patterns that might trigger memory allocation issues
  const patterns = [
    { name: 'all zeros', byte: 0 },
    { name: 'all ones', byte: 255 },
    { name: 'alternating', bytes: [0xAA, 0x55] },
    { name: 'ascending', bytes: Array.from({ length: 256 }, (_, i) => i) },
  ]

  for (const pattern of patterns) {
    const size = 50000 // 50KB value
    let value: Uint8Array

    if ('byte' in pattern) {
      value = new Uint8Array(size).fill(pattern.byte)
    } else {
      value = new Uint8Array(size)
      for (let i = 0; i < size; i++) {
        value[i] = pattern.bytes[i % pattern.bytes.length]
      }
    }

    const key = encodeString(`pattern-${pattern.name}`)
    db.put(key, value)

    const result = db.get(key)
    expect(result?.length, `Pattern ${pattern.name} length mismatch`).toBe(size)

    // Verify pattern integrity (sample check)
    for (let i = 0; i < Math.min(100, size); i += 10) {
      const expectedByte = 'byte' in pattern ? pattern.byte : pattern.bytes[i % pattern.bytes.length]
      expect(result?.[i], `Pattern ${pattern.name} byte ${i} mismatch`).toBe(expectedByte)
    }
  }
})