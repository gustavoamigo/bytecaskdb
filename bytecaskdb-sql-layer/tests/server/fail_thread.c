// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// LD_PRELOAD shim for test_wire.py: pthread_create fails with EAGAIN for the
// BYTECASK_FAIL_THREAD-th thread the process starts, as at a thread limit.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_int started;

int pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*routine)(void*), void* arg) {
  static int (*real)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
  if (!real) real = (int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*))dlsym(RTLD_NEXT, "pthread_create");
  const char* nth = getenv("BYTECASK_FAIL_THREAD");
  if (atomic_fetch_add(&started, 1) + 1 == (nth ? atoi(nth) : -1)) return EAGAIN;
  return real(thread, attr, routine, arg);
}
