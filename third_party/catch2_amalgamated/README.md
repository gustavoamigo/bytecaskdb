# Vendored Catch2 (amalgamated)

`catch_amalgamated.hpp`/`catch_amalgamated.cpp` are Catch2 v3.15.2's official
amalgamated distribution (Boost Software License 1.0, see the file headers),
pinned to match the version xmake's `catch2 3.x` package currently resolves
to. `catch2/catch_test_macros.hpp` and `catch2/generators/catch_generators.hpp`
are small shims so test files can keep using the normal modular Catch2
include paths against this single-TU build.

**Why this exists**: the `bytecask_tests_msan` target links against an
MSan-instrumented libc++ (see `scripts/build_msan_libcxx.sh`) built with
`-stdlib=libc++`. The prebuilt `catch2` xrepo package is compiled against the
system's default libstdc++, so its compiled static library exports
libstdc++-mangled symbols (`std::__cxx11::...`) for anything that crosses its
public API with a standard-library type (`std::string`, `SourceLineInfo`,
etc.) — those don't link against libc++-mangled references
(`std::__1::...`). Compiling Catch2 from source as ordinary translation units
of `bytecask_tests_msan` puts it under the exact same compiler invocation
(`-stdlib=libc++ -fsanitize=memory ...`) as the rest of the binary, so there
is no ABI boundary to cross. Every other target keeps using the normal
`catch2` package unchanged.

To refresh: download `extras/catch_amalgamated.{hpp,cpp}` from the matching
Catch2 release tag and replace the two files here.
