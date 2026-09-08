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

// Print memory usage after the program exits.
// Symbol.dispose / Symbol.iterator wiring lives in src/dispose.ts and is
// applied by src/wasm-backend.ts once the module has finished loading —
// shared with the native (N-API) backend so both behave identically.
Module.postRun = Module.postRun || [];
Module.postRun.push(function () {
  if (typeof process !== 'undefined' && process.memoryUsage) {
    var mem = process.memoryUsage();
    var MB = function (b) { return (b / 1048576).toFixed(1) + ' MiB'; };
    console.log('\n=== Memory Usage ===');
    console.log('  RSS:          ' + MB(mem.rss));
    console.log('  Heap total:   ' + MB(mem.heapTotal));
    console.log('  Heap used:    ' + MB(mem.heapUsed));
    console.log('  WASM memory:  ' + MB(HEAP8.byteLength));
    console.log('  External:     ' + MB(mem.external));
  }
});

