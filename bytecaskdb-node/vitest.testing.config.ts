import { defineConfig } from 'vitest/config'

// test/testing/ runs against the test-only builds (bytecask_testing.node,
// wasm/build/bytecask_testing.mjs), which carry the engine's fault
// checkpoints: `npm run test:testing`, BC_TEST_BACKEND=native for the addon.
export default defineConfig({
  test: {
    environment: 'node',
    pool: 'forks',
    include: ['test/testing/**/*.test.ts'],
    testTimeout: 10_000,
  },
  assetsInclude: ['**/*.wasm'],
})
