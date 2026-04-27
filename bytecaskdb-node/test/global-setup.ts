// Global setup for Vitest - validates WASM module before tests run
export async function setup() {
  // Validate the WASM binary can be loaded before any tests run
  // This gives a clean error message rather than 1000 test failures
  try {
    const { createWasmBackend } = await import('../src/wasm-backend.js')
    const backend = await createWasmBackend()
    console.log('✓ WASM module validated successfully')
    // Test basic factory functionality
    if (!backend.open || !backend.WritePlan) {
      throw new Error('WASM backend missing expected exports')
    }
  } catch (e) {
    throw new Error(`WASM failed to load before tests: ${e}`)
  }
}

export async function teardown() {
  // Any global cleanup (e.g., shared memory pools)
}