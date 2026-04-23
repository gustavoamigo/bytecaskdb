// Custom main wrapper that catches and prints C++ exceptions from benchmark.
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>

extern int main(int argc, char** argv);

// We rename the real main and call it from here.
// Actually, let's just use benchmark's Initialize + RunSpecifiedBenchmarks.
#include <benchmark/benchmark.h>

int main(int argc, char** argv) {
  try {
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
      return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "CAUGHT C++ exception: %s\n", e.what());
    return 1;
  } catch (...) {
    fprintf(stderr, "CAUGHT unknown C++ exception\n");
    return 1;
  }
}
