// Compiler for PHP (aka KPHP)
// Copyright (c) 2025 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

#include "runtime-light/k2-platform/k2-api.h"

namespace kphp::coro::detail {

class timer_handle {
  k2::descriptor m_descriptor{k2::INVALID_PLATFORM_DESCRIPTOR};
  k2::TimePoint m_time_point{};

public:
  timer_handle() noexcept = default;

  ~timer_handle() {
    clear();
  }

  timer_handle(const timer_handle&) = delete;
  timer_handle(timer_handle&&) = delete;
  timer_handle& operator=(const timer_handle&) = delete;
  timer_handle& operator=(timer_handle&&) = delete;

  auto clear() noexcept -> void {
    if (m_descriptor != k2::INVALID_PLATFORM_DESCRIPTOR) {
      k2::free_descriptor(std::exchange(m_descriptor, k2::INVALID_PLATFORM_DESCRIPTOR));
    }
    m_time_point = {};
  }

  auto reset(k2::TimePoint time_point) noexcept -> std::expected<void, int32_t> {
    clear();

    k2::TimePoint now{};
    k2::instant(std::addressof(now));
    if (time_point.time_point_ns <= now.time_point_ns) [[unlikely]] {
      time_point.time_point_ns = now.time_point_ns + 1;
    }

    if (const auto errc{k2::new_timer(std::addressof(m_descriptor), time_point.time_point_ns - now.time_point_ns)}; errc != k2::errno_ok) [[unlikely]] {
      return std::unexpected{errc};
    }
    m_time_point = time_point;
    return std::expected<void, int32_t>{};
  }

  auto descriptor() const noexcept -> k2::descriptor {
    return m_descriptor;
  }

  auto time_point() const noexcept -> k2::TimePoint {
    return m_time_point;
  }
};

} // namespace kphp::coro::detail
