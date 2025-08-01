// Compiler for PHP (aka KPHP)
// Copyright (c) 2024 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "common/mixin/not_copyable.h"
#include "runtime-common/core/allocator/script-allocator.h"
#include "runtime-common/core/std/containers.h"
#include "runtime-light/coroutine/shared-task.h"
#include "runtime-light/coroutine/task.h"
#include "runtime-light/stdlib/diagnostics/exception-types.h"
#include "runtime-light/stdlib/diagnostics/logs.h"

namespace kphp::forks {

inline constexpr int64_t INVALID_ID = -1;

} // namespace kphp::forks

struct ForkInstanceState final : private vk::not_copyable {
  // This is a temporary solution to destroy the fork state after the first call to f$wait.
  // In the future, we plan to implement a reference-counted future<T> that will automatically
  // manage and destroy the fork state once all referencing futures have been destroyed.
  struct fork_info final {
    bool awaited{};
    Throwable thrown_exception;
    std::optional<kphp::coro::shared_task<>> opt_handle;
  };

private:
  static constexpr int64_t FORK_ID_INIT = 0;

  int64_t next_fork_id{FORK_ID_INIT};
  // type erased tasks that represent forks
  kphp::stl::unordered_map<int64_t, fork_info, kphp::memory::script_allocator> forks;

public:
  int64_t current_id{FORK_ID_INIT};

  ForkInstanceState() noexcept = default;

  static ForkInstanceState& get() noexcept;

  template<typename return_type>
  std::pair<int64_t, kphp::coro::shared_task<return_type>> create_fork(kphp::coro::task<return_type> task) noexcept {
    const int64_t fork_id{next_fork_id++};
    auto fork_task{std::invoke(
        [](kphp::coro::task<return_type> task, int64_t fork_id) noexcept -> kphp::coro::shared_task<return_type> {
          ForkInstanceState::get().current_id = fork_id;
          co_return co_await std::move(task);
        },
        std::move(task), fork_id)};
    forks.emplace(fork_id, fork_info{.awaited = {}, .thrown_exception = {}, .opt_handle = static_cast<kphp::coro::shared_task<>>(fork_task)});
    return std::make_pair(fork_id, std::move(fork_task));
  }

  std::optional<std::reference_wrapper<fork_info>> get_info(int64_t fork_id) noexcept {
    if (auto it{forks.find(fork_id)}; it != forks.end()) [[likely]] {
      return it->second;
    }
    return std::nullopt;
  }

  std::reference_wrapper<fork_info> current_info() noexcept {
    auto opt_fork_info{get_info(current_id)};
    kphp::log::assertion(opt_fork_info.has_value());
    return *opt_fork_info;
  }
};
