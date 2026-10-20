// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// LeakSanitizer's suppressions for sql_layer_tests: only the engine's key
// directory node a failed allocation leaves unplaced, which it leaks rather
// than free while still reachable (#390). Every allocation of the SQL layer
// stays checked, under the allocation-failure sweeps too.

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
extern "C" auto __lsan_default_suppressions() -> const char*;  // NOLINT(bugprone-reserved-identifier): LSan's hook
extern "C" auto __lsan_default_suppressions() -> const char* { return "leak:btree_detail::NodePool\n"; }
#endif
#endif
