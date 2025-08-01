// Compiler for PHP (aka KPHP)
// Copyright (c) 2024 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <utility>

#include "runtime-light/coroutine/io-scheduler.h"
#include "runtime-light/coroutine/task.h"
#include "runtime-light/stdlib/diagnostics/logs.h"
#include "runtime-light/stdlib/fork/fork-functions.h"

template<std::invocable T>
void f$set_timer(int64_t timeout_ms, T&& on_timer_callback) noexcept {
  if (timeout_ms < 0) [[unlikely]] {
    kphp::log::warning("can't set timer for negative duration {}ms", timeout_ms);
    return;
  }
  // TODO choose from:
  // 1. someone should pop that fork from ForkInstanceState since it will stay there unless we perform f$wait on fork
  // 2. start timer_task via kphp::coro::io_scheduler::spawn (it won't have distinct fork id)
  auto timer_task{std::invoke(
      [](std::chrono::milliseconds duration, T on_timer_callback) noexcept -> kphp::coro::task<> {
        co_await kphp::forks::id_managed(kphp::coro::io_scheduler::get().schedule_after(duration));
        std::invoke(std::move(on_timer_callback));
      },
      std::chrono::milliseconds{timeout_ms}, std::forward<T>(on_timer_callback))};
  kphp::forks::start(std::move(timer_task));
}
