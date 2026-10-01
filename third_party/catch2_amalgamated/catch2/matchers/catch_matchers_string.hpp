// SPDX-License-Identifier: BSL-1.0
// Compatibility shim so tests can use the normal modular Catch2 include
// path (`<catch2/matchers/catch_matchers_string.hpp>`) against the vendored
// amalgamated source. See third_party/catch2_amalgamated/README.md for why
// this exists.
#pragma once
#include "../../catch_amalgamated.hpp"
