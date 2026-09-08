// Global setup for Vitest - validates the selected backend before tests run
export async function setup() {
  const backendName = process.env.BC_TEST_BACKEND === 'native' ? 'native' : 'wasm'
  try {
    const mod = backendName === 'native'
      ? await import('../src/native-backend.js')
      : await import('../src/wasm-backend.js')
    const backend = backendName === 'native'
      ? await mod.createNativeBackend()
      : await mod.createWasmBackend()
    console.log(`✓ ${backendName} module validated successfully`)
    // Test basic factory functionality
    if (!backend.open || !backend.WritePlan) {
      throw new Error(`${backendName} backend missing expected exports`)
    }
  } catch (e) {
    throw new Error(`${backendName} backend failed to load before tests: ${e}`)
  }
}

export async function teardown() {
  // Any global cleanup (e.g., shared memory pools)
}