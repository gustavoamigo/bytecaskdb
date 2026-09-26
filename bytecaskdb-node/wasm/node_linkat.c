// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Emscripten's __syscall_linkat is a weak stub that always fails with
// -EMLINK. This strong definition replaces it with a hard link through
// Node's fs (bc_node_linkat in syscall_overrides.js). Every WASM target runs
// under NODERAWFS, where paths are host paths and hard links are real.
#include <stdint.h>

int bc_node_linkat(int olddirfd, intptr_t oldpath, int newdirfd,
                   intptr_t newpath);

int __syscall_linkat(int olddirfd, intptr_t oldpath, int newdirfd,
                     intptr_t newpath, int flags) {
  (void)flags;  // AT_SYMLINK_FOLLOW: the engine only links regular files
  return bc_node_linkat(olddirfd, oldpath, newdirfd, newpath);
}
