// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <algorithm>
#include <cassert>
#include <thread>

#include "compiler/threading/locks.h"
#include "compiler/threading/thread-id.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

const int MAX_THREADS_COUNT = 102;

inline uint32_t get_default_threads_count() noexcept {
  return std::clamp(std::thread::hardware_concurrency(), 1U, static_cast<uint32_t>(MAX_THREADS_COUNT));
}

template<class T>
struct TLS {
private:
  struct TLSRaw {
    T data{};
    volatile int locker = 0;
    char dummy[4096];
  };

  // The thread with thread_id = 0 is the main thread in which the scheduler's master code is executed.
  // Threads with thread_id values in the range [1, MAX_THREADS_CNT] can be used as worker threads.
  // An additional thread with thread_id = MAX_THREADS_CNT + 1 can be used to work with the CppDestDirInitializer.
  // Therefore, the system requires a total of MAX_THREADS_CNT + 2 threads to be available.
  TLSRaw arr[MAX_THREADS_COUNT + 2];

  TLSRaw *get_raw(int id) {
    assert(0 <= id && id <= 1 + MAX_THREADS_COUNT);
    return &arr[id];
  }

  TLSRaw *get_raw() {
    return get_raw(get_thread_id());
  }

public:
  TLS() :
    arr() {
  }


  T &get() {
    return get_raw()->data;
  }

  T &get(int i) {
    return get_raw(i)->data;
  }

  T *operator->() {
    return &get();
  }

  T &operator*() {
    return get();
  }

  int size() {
    return MAX_THREADS_COUNT + 1;
  }

  T *lock_get() {
    TLSRaw *raw = get_raw();
    bool ok = try_lock(&raw->locker);
    assert(ok);
    return &raw->data;
  }

  void unlock_get(T *ptr) {
    TLSRaw *raw = get_raw();
    assert(&raw->data == ptr);
    unlock(&raw->locker);
  }
};

#pragma GCC diagnostic pop
