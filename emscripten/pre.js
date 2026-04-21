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

// Wire up Symbol.dispose on Embind classes so `using db = ...` works
// (TC39 Explicit Resource Management, Node.js 22+, TypeScript 5.2+).
Module.onRuntimeInitialized = Module.onRuntimeInitialized || function () {};
var origInit = Module.onRuntimeInitialized;
Module.onRuntimeInitialized = function () {
  origInit.call(this);
  var classes = ['ByteCaskDB', 'Snapshot', 'WritePlan'];
  for (var i = 0; i < classes.length; i++) {
    var cls = Module[classes[i]];
    if (cls && cls.prototype && typeof Symbol !== 'undefined' && Symbol.dispose) {
      cls.prototype[Symbol.dispose] = cls.prototype.close || cls.prototype.delete;
    }
  }
};
