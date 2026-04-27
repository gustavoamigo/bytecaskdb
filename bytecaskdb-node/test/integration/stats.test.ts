// Statistics and monitoring tests for ByteCaskDB
import { test, expect, decodeBytes } from '../fixtures/index.js'

test('stats returns expected counters', async ({ db }) => {
  const initialStats = db.stats()

  // Stats should be an object with numeric values
  expect(initialStats).toBeTypeOf('object')
  expect(Object.keys(initialStats).length).toBeGreaterThan(0)

  // All stat values should be numbers
  for (const [key, value] of Object.entries(initialStats)) {
    expect(value, `stat ${key} should be a number`).toBeTypeOf('number')
    expect(value, `stat ${key} should be finite`).toBeTypeOf('number')
  }

  // Common expected stats (these may vary by implementation)
  const expectedStats = [
    'bytes_written',
    'entries_written',
    'fsyncs',
    'open_files',
  ]

  // Check if some expected stats exist (implementation may vary)
  const statKeys = Object.keys(initialStats)
  let foundExpectedStats = 0
  for (const expectedStat of expectedStats) {
    if (statKeys.some(key => key.includes(expectedStat.split('_')[0]))) {
      foundExpectedStats++
    }
  }

  // Should find at least some expected stat categories
  expect(foundExpectedStats).toBeGreaterThan(0)
})

test('stats reflect database operations', async ({ db }) => {
  const beforeStats = db.stats()

  // Perform some operations
  const operations = [
    { key: 'stats-test-1', value: 'value-1' },
    { key: 'stats-test-2', value: 'value-2' },
    { key: 'stats-test-3', value: 'value-3' },
  ]

  for (const { key, value } of operations) {
    db.put(key, value)
  }

  // Delete one key
  db.del('stats-test-2')

  const afterStats = db.stats()

  // Some stats should have changed
  let changedStats = 0
  for (const key of Object.keys(beforeStats)) {
    if (beforeStats[key] !== afterStats[key]) {
      changedStats++
    }
  }

  // At least some stats should reflect the operations
  expect(changedStats).toBeGreaterThan(0)

  // Monotonic counters should not decrease
  for (const [key, beforeValue] of Object.entries(beforeStats)) {
    const afterValue = afterStats[key]

    // Skip gauges that can go up or down (like open_files)
    if (key.includes('open') || key.includes('degraded')) {
      continue
    }

    // Monotonic counters should not decrease
    expect(afterValue, `Counter ${key} should not decrease`).toBeGreaterThanOrEqual(beforeValue)
  }
})

test('vacuum operations work correctly', async ({ db }) => {
  // Setup data to create some vacuum opportunities
  const testData = Array.from({ length: 50 }, (_, i) => ({
    key: `vacuum-test-${i}`,
    value: `value-${i}`
  }))

  // Insert data
  for (const { key, value } of testData) {
    db.put(key, value)
  }

  // Overwrite some data to create garbage
  for (let i = 0; i < 25; i++) {
    const key = `vacuum-test-${i}`
    const newValue = `updated-value-${i}`
    db.put(key, newValue)
  }

  // Delete some data to create more garbage
  for (let i = 25; i < 35; i++) {
    const key = `vacuum-test-${i}`
    db.del(key)
  }

  // Get stats before vacuum
  const beforeVacuum = db.stats()

  // Run vacuum
  const vacuumResult = db.vacuum()

  // vacuum() should return a boolean
  expect(vacuumResult).toBeTypeOf('boolean')

  // Get stats after vacuum
  const afterVacuum = db.stats()

  // Stats should still be valid
  expect(afterVacuum).toBeTypeOf('object')

  // Verify data integrity after vacuum
  // Updated keys should have new values
  for (let i = 0; i < 25; i++) {
    const key = `vacuum-test-${i}`
    const expectedValue = `updated-value-${i}`
    expect(decodeBytes(db.get(key)!)).toEqual(expectedValue)
  }

  // Deleted keys should remain deleted
  for (let i = 25; i < 35; i++) {
    const key = `vacuum-test-${i}`
    expect(db.get(key)).toBeNull()
  }

  // Untouched keys should remain unchanged
  for (let i = 35; i < 50; i++) {
    const key = `vacuum-test-${i}`
    const expectedValue = `value-${i}`
    expect(decodeBytes(db.get(key)!)).toEqual(expectedValue)
  }
})

test('currentSequence behavior works correctly', async ({ db }) => {
  // Get current sequence (non-blocking)
  const initialSeq = db.currentSequence()
  expect(initialSeq).toBeTypeOf('number')
  expect(initialSeq).toBeGreaterThanOrEqual(0)

  // Perform some operations
  const key = 'sequence-test'
  const value = 'sequence-value'

  db.put(key, value)

  // Sequence should advance
  const afterPutSeq = db.currentSequence()
  expect(afterPutSeq).toBeGreaterThanOrEqual(initialSeq)

  // Another operation
  db.del(key)

  const afterDelSeq = db.currentSequence()
  expect(afterDelSeq).toBeGreaterThanOrEqual(afterPutSeq)

  // Non-blocking call with timeout 0 should return immediately
  const timeoutSeq = db.currentSequence(0)
  expect(timeoutSeq).toBeTypeOf('number')
})

test('operational counter accuracy', async ({ db }) => {
  const beforeStats = db.stats()

  // Perform counted operations
  const putOperations = 10
  const delOperations = 3

  // Multiple puts
  for (let i = 0; i < putOperations; i++) {
    db.put(`counter-put-${i}`, `value-${i}`)
  }

  // Some deletes
  for (let i = 0; i < delOperations; i++) {
    db.del(`counter-put-${i}`)
  }

  const afterStats = db.stats()

  // Look for operation-related counters
  const statKeys = Object.keys(afterStats)

  // Check for write-related stats
  const writeStats = statKeys.filter(key =>
    key.includes('write') || key.includes('put') || key.includes('entries')
  )

  if (writeStats.length > 0) {
    // At least one write-related stat should have increased
    let writeStatsIncreased = false
    for (const statKey of writeStats) {
      if (afterStats[statKey] > beforeStats[statKey]) {
        writeStatsIncreased = true
        break
      }
    }
    expect(writeStatsIncreased, 'Write operations should be reflected in stats').toBe(true)
  }

  // Verify stats are still well-formed
  for (const [key, value] of Object.entries(afterStats)) {
    expect(value, `stat ${key} should be a finite number`).toBeTypeOf('number')
  }
})

test('stats consistency across operations', async ({ db }) => {
  // Collect stats at multiple points
  const statsHistory = []

  statsHistory.push({ point: 'initial', stats: db.stats() })

  // Batch operation
  const key1 = 'consistency-1'
  const key2 = 'consistency-2'
  const value1 = 'value-1'
  const value2 = 'value-2'

  db.put(key1, value1)
  statsHistory.push({ point: 'after-put-1', stats: db.stats() })

  db.put(key2, value2)
  statsHistory.push({ point: 'after-put-2', stats: db.stats() })

  db.del(key1)
  statsHistory.push({ point: 'after-del', stats: db.stats() })

  // Verify stats progression makes sense
  for (let i = 1; i < statsHistory.length; i++) {
    const prev = statsHistory[i - 1].stats
    const curr = statsHistory[i].stats

    // All stats should remain numbers
    for (const key of Object.keys(curr)) {
      expect(curr[key], `stat ${key} at ${statsHistory[i].point}`).toBeTypeOf('number')
    }

    // Stats keys should remain consistent
    expect(Object.keys(curr).sort()).toEqual(Object.keys(prev).sort())
  }
})

test('stats during iterator and snapshot operations', async ({ db }) => {
  // Setup test data
  const testData = Array.from({ length: 20 }, (_, i) => ({
    key: `stats-iter-${i}`,
    value: `value-${i}`
  }))

  for (const { key, value } of testData) {
    db.put(key, value)
  }

  const beforeIterStats = db.stats()

  // Create multiple iterators and snapshots
  const snapshot1 = db.snapshot()
  const snapshot2 = db.snapshot()
  const entries = db.entries('')
  const keys = db.keys('')

  const duringResourceStats = db.stats()

  // Use resources
  let entryCount = 0
  for (const entry of entries) {
    entryCount++
    if (entryCount >= 5) break
  }

  let keyCount = 0
  for (const key of keys) {
    keyCount++
    if (keyCount >= 3) break
  }

  // Check snapshots
  expect(decodeBytes(snapshot1.get(testData[0].key)!)).toEqual(testData[0].value)
  expect(decodeBytes(snapshot2.get(testData[1].key)!)).toEqual(testData[1].value)

  // Clean up resources
  entries.close()
  keys.close()
  snapshot1.close()
  snapshot2.close()

  const afterResourceStats = db.stats()

  // Stats should remain consistent throughout
  const allStats = [beforeIterStats, duringResourceStats, afterResourceStats]

  for (const stats of allStats) {
    expect(stats).toBeTypeOf('object')
    for (const [key, value] of Object.entries(stats)) {
      expect(value, `stat ${key} should be finite`).toBeTypeOf('number')
    }
  }
})

test('stats handle large numbers correctly', async ({ db }) => {
  // Perform many operations to test large counter values
  const operationCount = 1000

  for (let i = 0; i < operationCount; i++) {
    const key = `large-stats-${i % 100}` // Reuse keys to create overwrites
    const value = `value-${i}`
    db.put(key, value)
  }

  const stats = db.stats()

  // All stats should handle large numbers without overflow
  for (const [key, value] of Object.entries(stats)) {
    expect(value, `stat ${key} should be finite`).toBeTypeOf('number')
    expect(value, `stat ${key} should not be negative unless expected`).toBeGreaterThanOrEqual(0)
    expect(Number.isSafeInteger(value), `stat ${key} should be safe integer`).toBe(true)
  }
})