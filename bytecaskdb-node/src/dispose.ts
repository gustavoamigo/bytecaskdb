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
  // The DB first runs its own close (closeDb); the handle is deleted whether
  // or not that throws.
  //
  // A deleted Embind handle makes every bound method throw Embind's own
  // BindingError. Check first, so a closed handle reports BC_CLOSED as it
  // does on the native addon, and a closed iterator is done, as it is there.
  // Embind also converts the arguments before the C++ guard runs, so an
  // argument of the wrong type is its BindingError (or a TypeError, for a
  // value that is not a BigInt), with no code: give it BC_INVALID_ARGUMENT,
  // as the native addon does. Anything else uncoded, such as a WebAssembly
  // trap, is BC_RUNTIME. The C++ guard's own errors already carry a code.
  for (const name of DISPOSABLE_CLASSES) {
    const cls = module[name];
    if (typeof cls?.prototype?.delete !== "function") continue;
    const proto = cls.prototype;
    const isIterator = ITERATOR_CLASSES.includes(name);
    for (const method of Object.getOwnPropertyNames(proto)) {
      const bound = proto[method];
      if (method === "constructor" || typeof bound !== "function") continue;
      proto[method] = function (this: { isDeleted(): boolean }, ...args: unknown[]) {
        if (this.isDeleted()) {
          if (isIterator && method === "next") return { done: true, value: undefined };
          throw closedError(name);
        }
        return withCode(() => bound.apply(this, args));
      };
    }
    for (const fn of Object.getOwnPropertyNames(cls)) {
      const bound = cls[fn];
      if (typeof bound !== "function") continue;
      cls[fn] = (...args: unknown[]) => withCode(() => bound(...args));
    }
    proto.close = function () {
      if (this.isDeleted()) return;
      try {
        if (typeof this.closeDb === "function") this.closeDb();
      } finally {
        this.delete();
      }
    };
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

function withCode<T>(call: () => T): T {
  try {
    return call();
  } catch (e) {
    if (e instanceof Error && !("code" in e)) {
      const badArgument = e.name === "BindingError" || e instanceof TypeError;
      Object.assign(e, { code: badArgument ? "BC_INVALID_ARGUMENT" : "BC_RUNTIME" });
    }
    throw e;
  }
}

function closedError(name: string): Error {
  return Object.assign(new Error(`${name} is closed`), { code: "BC_CLOSED" });
}
