// Compiler for PHP (aka KPHP)
// Copyright (c) 2025 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "runtime-light/stdlib/diagnostics/logs.h"

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "runtime-light/k2-platform/k2-api.h"
#include "runtime-light/stdlib/diagnostics/backtrace.h"

namespace kphp::log::impl::details {

void log(level level, std::optional<std::span<void* const>> trace, std::string_view message) noexcept {
  if (!trace.has_value()) {
    k2::log(std::to_underlying(level), message, std::nullopt);
    return;
  }
  std::array<k2::LogTaggedEntry, 1> tagged_entries; // NOLINT

  static constexpr std::string_view BACKTRACE_KEY = "trace";
  static constexpr size_t BACKTRACE_BUFFER_SIZE = 1024UZ * 4UZ;
  std::array<char, BACKTRACE_BUFFER_SIZE> backtrace_buffer; // NOLINT
  std::string_view backtrace{"[]"};
  if (auto backtrace_symbols{kphp::diagnostic::backtrace_symbols(*trace)}; !backtrace_symbols.empty()) {
    const auto [trace_out, trace_size]{std::format_to_n(backtrace_buffer.data(), backtrace_buffer.size() - 1, "\n{}", backtrace_symbols)};
    *trace_out = '\0';
    backtrace = std::string_view{backtrace_buffer.data(), static_cast<std::string_view::size_type>(trace_size)};
  } else if (auto backtrace_addresses{kphp::diagnostic::backtrace_addresses(*trace)}; !backtrace_addresses.empty()) {
    const auto [trace_out, trace_size]{std::format_to_n(backtrace_buffer.data(), backtrace_buffer.size() - 1, "{}", backtrace_addresses)};
    *trace_out = '\0';
    backtrace = std::string_view{backtrace_buffer.data(), static_cast<std::string_view::size_type>(trace_size)};
  }
  tagged_entries[0] =
      k2::LogTaggedEntry{.key = BACKTRACE_KEY.data(), .value = backtrace.data(), .key_len = BACKTRACE_KEY.size(), .value_len = backtrace.size()};
  k2::log(std::to_underlying(level), message, tagged_entries);
}

} // namespace kphp::log::impl::details
