// Propagate host environment variables into the WASM module so that
// std::getenv picks up values set in the shell (e.g. BC_DATASET_SIZE).
Module.preRun = Module.preRun || [];
Module.preRun.push(function () {
  if (typeof process !== 'undefined' && process.env) {
    for (var key in process.env) {
      ENV[key] = process.env[key];
    }
  }
});

// Wire up Symbol.dispose and Symbol.iterator on Embind classes.
Module.onRuntimeInitialized = Module.onRuntimeInitialized || function () {};
var origInit = Module.onRuntimeInitialized;
Module.onRuntimeInitialized = function () {
  origInit.call(this);

  // Symbol.dispose — explicit resource management (Node.js 22+, TC39)
  var disposableClasses = ['ByteCaskDB', 'Snapshot', 'WritePlan',
      'EntryIterator', 'KeyIterator', 'ReverseEntryIterator', 'ReverseKeyIterator'];
  for (var i = 0; i < disposableClasses.length; i++) {
    var cls = Module[disposableClasses[i]];
    if (cls && cls.prototype && typeof Symbol !== 'undefined' && Symbol.dispose) {
      cls.prototype[Symbol.dispose] = cls.prototype.close || cls.prototype.delete;
    }
  }

  // Symbol.iterator — JS iterator protocol for scan classes
  var iteratorClasses = ['EntryIterator', 'KeyIterator',
      'ReverseEntryIterator', 'ReverseKeyIterator'];
  for (var i = 0; i < iteratorClasses.length; i++) {
    var cls = Module[iteratorClasses[i]];
    if (cls && cls.prototype && typeof Symbol !== 'undefined' && Symbol.iterator) {
      cls.prototype[Symbol.iterator] = function () { return this; };
    }
  }
};
