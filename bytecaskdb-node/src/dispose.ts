// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

// Shared Symbol.dispose / Symbol.iterator wiring for both backends.
// The WASM (Embind) and native (N-API) modules each expose the same set of
// classes with the same method names (close(), next()), so the wiring itself
// is backend-neutral — only how the module is loaded differs.

const DISPOSABLE_CLASSES = [
  "ByteCaskDB",
  "Snapshot",
  "WritePlan",
  "EntryIterator",
  "KeyIterator",
  "ReverseEntryIterator",
  "ReverseKeyIterator",
  "ChangeIterator",
  "FileManifest",
];

const ITERATOR_CLASSES = [
  "EntryIterator",
  "KeyIterator",
  "ReverseEntryIterator",
  "ReverseKeyIterator",
  "ChangeIterator",
];

// eslint-disable-next-line @typescript-eslint/no-explicit-any
export function applyDisposeWiring(module: Record<string, any>): void {
  if (typeof Symbol === "undefined") return;

  // Embind owns a ClassHandle's native pointer. Deleting it from a bound C++
  // close() method leaves that handle live, so Embind later deletes the same
  // pointer again. Give Embind classes a close() that delegates to their own
  // idempotent delete() lifecycle API. N-API classes already expose close().
  for (const name of DISPOSABLE_CLASSES) {
    const cls = module[name];
    if (typeof cls?.prototype?.delete === "function") {
      cls.prototype.close = function () {
        if (!this.isDeleted()) {
          this.delete();
        }
      };
    }
  }

  if (Symbol.dispose) {
    for (const name of DISPOSABLE_CLASSES) {
      const cls = module[name];
      if (cls?.prototype) {
        cls.prototype[Symbol.dispose] = cls.prototype.close;
      }
    }
  }

  if (Symbol.iterator) {
    for (const name of ITERATOR_CLASSES) {
      const cls = module[name];
      if (cls?.prototype) {
        cls.prototype[Symbol.iterator] = function () {
          return this;
        };
      }
    }
  }
}
