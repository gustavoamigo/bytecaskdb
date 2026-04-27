// Temporary directory utilities for testing
import { mkdtemp, rm } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

/**
 * Creates a temporary directory with a given prefix and automatic cleanup.
 * Returns a promise that resolves to [path, cleanup_function].
 */
export async function createTempDir(prefix: string = 'bytecask-test-'): Promise<[string, () => Promise<void>]> {
  const dir = await mkdtemp(join(tmpdir(), prefix))

  const cleanup = async () => {
    try {
      await rm(dir, { recursive: true, force: true })
    } catch (error) {
      // Ignore cleanup errors in tests
      console.warn(`Failed to clean up temp dir ${dir}:`, error)
    }
  }

  return [dir, cleanup]
}

/**
 * Creates multiple temporary directories for tests that need isolation.
 */
export async function createTempDirs(count: number, prefix: string = 'bytecask-test-'): Promise<Array<[string, () => Promise<void>]>> {
  const dirs: Array<[string, () => Promise<void>]> = []

  for (let i = 0; i < count; i++) {
    dirs.push(await createTempDir(`${prefix}${i}-`))
  }

  return dirs
}