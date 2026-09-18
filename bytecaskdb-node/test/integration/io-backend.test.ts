// IoBackend / BufferPoolOptions tests for ByteCaskDB (buffer pool bindings).
//
// The WASM backend only supports IoBackend.Pread: DB::open rejects both
// 'mmap' and 'bufferPool' on Emscripten builds (mmap emulation would
// double-buffer into the WASM heap, and MEMFS is already memory, so there
// is no page cache for the pool to bound — see docs/buffer_pool_design.md).
// The native backend supports all three.
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

test.skipIf(isNative)('bufferPool ioBackend is rejected on the WASM backend', async ({ tmpDir, wasmBackend }) => {
  const dbPath = join(tmpDir, 'buffer-pool-io-backend-wasm.db')
  expect(() =>
    wasmBackend.open(dbPath, {
      ioBackend: 'bufferPool',
      bufferPool: { capacityBytes: 16 * 1024 * 1024 },
    }),
  ).toThrow()
})

test.skipIf(!isNative)(
  'bufferPool ioBackend rejects a capacity below 2x maxFileBytes',
  async ({ tmpDir, wasmBackend }) => {
    const dbPath = join(tmpDir, 'buffer-pool-too-small.db')
    expect(() =>
      wasmBackend.open(dbPath, {
        maxFileBytes: 4 * 1024 * 1024,
        ioBackend: 'bufferPool',
        bufferPool: { capacityBytes: 1024 * 1024 },
      }),
    ).toThrow(/buffer.?pool|capacity/i)
  },
)

test.skipIf(!isNative)(
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
