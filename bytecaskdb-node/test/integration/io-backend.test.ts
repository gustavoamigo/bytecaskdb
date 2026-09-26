// IoBackend / BufferPoolOptions tests for ByteCaskDB (buffer pool bindings).
//
// The native backend supports all three back-ends. The WASM backend supports
// 'pread' and 'bufferPool' and rejects 'mmap': mmap emulation would
// double-buffer the data file into the WASM heap. There the pool's value is
// that a hit skips the per-read call out to Node's fs.
import { join } from 'node:path'
import { test, expect, encodeString, decodeBytes } from '../fixtures/index.js'

const isNative = process.env.BC_TEST_BACKEND === 'native'

test('invalid ioBackend value throws', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'invalid-io-backend.db')
  expect(() => wasmBackend.open(dbPath, { ioBackend: 'nonsense' as any })).toThrow()
})

test('explicit pread ioBackend round-trips like the default', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'pread-io-backend.db')
  const db = wasmBackend.open(dbPath, { ioBackend: 'pread' })
  try {
    db.put(encodeString('k'), encodeString('v'))
    expect(decodeBytes(db.get(encodeString('k'))!)).toEqual('v')
  } finally {
    await db.close()
  }
})

test.skipIf(!isNative)('mmap ioBackend works on the native backend', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'mmap-io-backend.db')
  const db = wasmBackend.open(dbPath, { ioBackend: 'mmap' })
  try {
    db.put(encodeString('k'), encodeString('v'))
    expect(decodeBytes(db.get(encodeString('k'))!)).toEqual('v')
  } finally {
    await db.close()
  }
})

test.skipIf(isNative)('mmap ioBackend is rejected on the WASM backend', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'mmap-io-backend-wasm.db')
  expect(() => wasmBackend.open(dbPath, { ioBackend: 'mmap' })).toThrow()
})

test(
  'bufferPool ioBackend rejects a capacity below 2x maxFileBytes',
  async ({ tmpDir, wasmBackend }) => {
    const dbPath = join(tmpDir, 'buffer-pool-too-small.db')
    const open = () =>
      wasmBackend.open(dbPath, {
        maxFileBytes: 4 * 1024 * 1024,
        ioBackend: 'bufferPool',
        bufferPool: { capacityBytes: 1024 * 1024 },
      })
    // The WASM backend surfaces C++ exceptions without their message.
    if (isNative) expect(open).toThrow(/buffer.?pool|capacity/i)
    else expect(open).toThrow()
  },
)

test(
  'bufferPool ioBackend serves reads and reports pool counters in stats',
  async ({ tmpDir, wasmBackend }) => {
    const dbPath = join(tmpDir, 'buffer-pool.db')
    const maxFileBytes = 4 * 1024 * 1024
    const db = wasmBackend.open(dbPath, {
      maxFileBytes,
      ioBackend: 'bufferPool',
      bufferPool: { capacityBytes: 2 * maxFileBytes, directIo: false },
    })
    try {
      for (let i = 0; i < 50; i++) {
        db.put(encodeString(`key-${i}`), encodeString(`value-${i}`))
      }
      for (let i = 0; i < 50; i++) {
        expect(decodeBytes(db.get(encodeString(`key-${i}`))!)).toEqual(`value-${i}`)
      }

      const stats = db.stats()
      for (const counter of [
        'bytecask.pool_hits',
        'bytecask.pool_misses',
        'bytecask.pool_frames_total',
      ]) {
        expect(stats, `stats should report ${counter}`).toHaveProperty(counter)
        expect(stats[counter]).toBeTypeOf('number')
      }
    } finally {
      await db.close()
    }
  },
)
