// Custom matchers for ByteCaskDB testing
import { expect } from 'vitest'

// Helper function to convert Uint8Array to hex string
function bufToHex(buffer: Uint8Array): string {
  return Array.from(buffer)
    .map(b => b.toString(16).padStart(2, '0'))
    .join('')
    .toUpperCase()
}

// Helper function to convert hex string to Uint8Array
function hexToBuf(hexString: string): Uint8Array {
  const cleanHex = hexString.replace(/\s/g, '').toUpperCase()
  return Uint8Array.from(
    cleanHex.match(/.{2}/g)!.map(byte => parseInt(byte, 16))
  )
}

declare module 'vitest' {
  interface Assertion<T = any> {
    toMatchBinary(hexString: string): T
    toYieldValues<U>(expected: U[]): Promise<T>
  }
}

expect.extend({
  toMatchBinary(received: Uint8Array, hexString: string) {
    const expected = hexToBuf(hexString)
    const pass = received.length === expected.length &&
      received.every((byte, i) => byte === expected[i])

    return {
      pass,
      message: () => pass
        ? `Expected buffer NOT to match hex ${hexString}`
        : `Expected ${bufToHex(received)} to match hex ${hexString}`,
    }
  },

  async toYieldValues<T>(iterator: AsyncIterable<T>, expected: T[]) {
    const actual: T[] = []
    for await (const item of iterator) {
      actual.push(item)
    }

    const pass = this.equals(actual, expected)
    return {
      pass,
      message: () => pass
        ? `Expected iterator NOT to yield ${JSON.stringify(expected)}`
        : `Expected iterator to yield ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`,
    }
  }
})