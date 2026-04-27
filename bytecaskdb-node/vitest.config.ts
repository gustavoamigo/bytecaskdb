import { defineConfig } from 'vitest/config'

export default defineConfig({
  test: {
    // Node environment — no DOM needed for a DB library
    environment: 'node',

    // forks pool: safest for WASM + native addons; each file in own process
    pool: 'forks',

    // Global setup: validate WASM binary is loadable before any tests run
    globalSetup: ['./test/global-setup.ts'],

    // Per-file setup: register custom matchers
    setupFiles: ['./test/helpers/matchers.ts'],

    // Detect leaked async handles (Vitest v4+)
    // Catches unclosed DB handles, dangling WASM memory, etc.
    detectOpenHandles: true,

    // Test file pattern
    include: ['test/**/*.test.ts'],
    exclude: ['test/fixtures/**', 'test/helpers/**'],

    // Timeouts — async init + WASM can be slow
    testTimeout: 10_000,
    hookTimeout: 15_000,

    coverage: {
      provider: 'v8',
      reporter: ['text', 'html', 'lcov'],
      include: ['src/**/*.ts'],
      exclude: ['src/**/*.d.ts'],
      thresholds: {
        lines: 90,
        branches: 85,
        functions: 95,
        statements: 90
      },
    },
  },
  // Tell Vitest's asset pipeline to pass .wasm files through untransformed
  assetsInclude: ['**/*.wasm'],
})