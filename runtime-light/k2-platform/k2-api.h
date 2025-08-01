// Compiler for PHP (aka KPHP)
// Copyright (c) 2024 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <sys/utsname.h>
#include <utility>

#define K2_API_HEADER_H
#include "runtime-light/k2-platform/k2-header.h"
#undef K2_API_HEADER_H

namespace k2 {

namespace k2_impl_ {

inline constexpr size_t DEFAULT_MEMORY_ALIGN = 16;

} // namespace k2_impl_

inline constexpr int32_t errno_ok = 0;
inline constexpr int32_t errno_enodev = ENODEV;
inline constexpr int32_t errno_einval = EINVAL;
inline constexpr int32_t errno_enodata = ENODATA;
inline constexpr int32_t errno_efault = EFAULT;
inline constexpr int32_t errno_enomem = ENOMEM;
inline constexpr int32_t errno_etimedout = ETIMEDOUT;
inline constexpr int32_t errno_eshutdown = ESHUTDOWN;
inline constexpr int32_t errno_ecanceled = ECANCELED;

using descriptor = uint64_t;
inline constexpr k2::descriptor INVALID_PLATFORM_DESCRIPTOR = 0;

enum class stream_kind : uint8_t { component, udp, tcp };

using IOStatus = IOStatus;

using StreamStatus = StreamStatus;

using UpdateStatus = UpdateStatus;

using TimePoint = TimePoint;

using SystemTime = SystemTime;

using PollStatus = PollStatus;

using ImageInfo = ImageInfo;

using ControlFlags = ControlFlags;

inline const ImageInfo* describe() noexcept {
  return k2_describe();
}

inline const ControlFlags* control_flags() noexcept {
  return k2_control_flags();
}

inline const ImageState* image_state() noexcept {
  return k2_image_state();
}

inline const ComponentState* component_state() noexcept {
  return k2_component_state();
}

inline InstanceState* instance_state() noexcept {
  return k2_instance_state();
}

inline void* alloc_align(size_t size, size_t align) noexcept {
  return k2_alloc(size, align);
}

inline void* alloc(size_t size) noexcept {
  return k2::alloc_align(size, k2_impl_::DEFAULT_MEMORY_ALIGN);
}

inline void* realloc(void* ptr, size_t new_size) noexcept {
  return k2_realloc(ptr, new_size);
}

inline void* realloc_checked(void* ptr, size_t old_size, size_t align, size_t new_size) noexcept {
  return k2_realloc_checked(ptr, old_size, align, new_size);
}

inline void free(void* ptr) noexcept {
  k2_free(ptr);
}

inline void free_checked(void* ptr, size_t size, size_t align) noexcept {
  k2_free_checked(ptr, size, align);
}

[[noreturn]] inline void exit(int32_t exit_code) noexcept {
  k2_exit(exit_code);
}

inline uint32_t getpid() noexcept {
  return k2_getpid();
}

inline int32_t uname(struct utsname* addr) noexcept {
  return k2_uname(addr);
}

inline int32_t open(k2::descriptor* descriptor, size_t name_len, const char* name) noexcept {
  return k2_open(descriptor, name_len, name);
}

inline int32_t access(std::string_view component_name) noexcept {
  return k2_access(component_name.size(), component_name.data());
}

inline void stream_status(k2::descriptor descriptor, StreamStatus* status) noexcept {
  k2_stream_status(descriptor, status);
}

inline size_t write(k2::descriptor descriptor, size_t data_len, const void* data) noexcept {
  return k2_write(descriptor, data_len, data);
}

inline size_t stderr_write(size_t data_len, const void* data) noexcept {
  return k2_stderr_write(data_len, data);
}

inline size_t read(k2::descriptor descriptor, size_t buf_len, void* buf) noexcept {
  return k2_read(descriptor, buf_len, buf);
}

inline void please_shutdown(k2::descriptor descriptor) noexcept {
  k2_please_shutdown(descriptor);
}

inline void shutdown_write(k2::descriptor descriptor) noexcept {
  k2_shutdown_write(descriptor);
}

inline void instant(TimePoint* time_point) noexcept {
  k2_instant(time_point);
}

inline int32_t new_timer(k2::descriptor* descriptor, uint64_t duration_ns) noexcept {
  return k2_new_timer(descriptor, duration_ns);
}

inline int32_t timer_deadline(k2::descriptor descriptor, TimePoint* deadline) noexcept {
  return k2_timer_deadline(descriptor, deadline);
}

inline void free_descriptor(k2::descriptor descriptor) noexcept {
  k2_free_descriptor(descriptor);
}

inline k2::UpdateStatus take_update(k2::descriptor* descriptor) noexcept {
  return k2_take_update(descriptor);
}

using LogTaggedEntry = ::LogKeyValuePair;

inline void log(size_t level, std::string_view msg, std::optional<std::span<LogTaggedEntry>> tags) noexcept {
  const auto tags_count{tags.value_or(std::span<LogTaggedEntry>{}).size()};
  const auto* tags_data{tags.value_or(std::span<LogTaggedEntry>{}).data()};
  k2_log(level, msg.size(), msg.data(), tags_count, tags_data);
}

inline size_t log_level_enabled() noexcept {
  return k2_log_level_enabled();
}

inline void os_rnd(size_t len, void* bytes) noexcept {
  k2_os_rnd(len, bytes);
}

inline void system_time(SystemTime* system_time) noexcept {
  k2_system_time(system_time);
}

inline uint32_t args_count() noexcept {
  return k2_args_count();
}

inline auto arg_fetch(uint32_t arg_num) noexcept {
  const auto key_len{k2_args_key_len(arg_num)};
  const auto value_len{k2_args_value_len(arg_num)};

  // +1 since we get non-null-terminated strings from platform and we want to null-terminate them on our side
  auto* key_buffer{static_cast<char*>(k2::alloc(key_len + 1))};
  auto* value_buffer{static_cast<char*>(k2::alloc(value_len + 1))};
  k2_args_fetch(arg_num, key_buffer, value_buffer);
  // null-terminate
  key_buffer[key_len] = '\0';
  value_buffer[value_len] = '\0';

  return std::make_pair(std::unique_ptr<char, decltype(std::addressof(k2::free))>{key_buffer, k2::free},
                        std::unique_ptr<char, decltype(std::addressof(k2::free))>{value_buffer, k2::free});
}

inline uint32_t env_count() noexcept {
  return k2_env_count();
}

inline auto env_fetch(uint32_t arg_num) noexcept {
  const auto key_len{k2_env_key_len(arg_num)};
  const auto value_len{k2_env_value_len(arg_num)};

  // +1 since we get non-null-terminated strings from platform and we want to null-terminate them on our side
  auto* key_buffer{static_cast<char*>(k2::alloc(key_len + 1))};
  auto* value_buffer{static_cast<char*>(k2::alloc(value_len + 1))};
  k2_env_fetch(arg_num, key_buffer, value_buffer);
  // null-terminate
  key_buffer[key_len] = '\0';
  value_buffer[value_len] = '\0';

  return std::make_pair(std::unique_ptr<char, decltype(std::addressof(k2::free))>{key_buffer, k2::free},
                        std::unique_ptr<char, decltype(std::addressof(k2::free))>{value_buffer, k2::free});
}

inline int32_t set_timezone(std::string_view timezone) noexcept {
  return k2_set_timezone(timezone.data());
}

inline struct tm* localtime_r(const time_t* timer, struct tm* result) noexcept {
  return k2_localtime_r(timer, result);
}

inline int32_t udp_connect(k2::descriptor* descriptor, const char* host, size_t host_len) noexcept {
  return k2_udp_connect(descriptor, host, host_len);
}

inline int32_t tcp_connect(k2::descriptor* descriptor, const char* host, size_t host_len) noexcept {
  return k2_tcp_connect(descriptor, host, host_len);
}

inline int32_t iconv_open(void** iconv_cd, const char* tocode, const char* fromcode) noexcept {
  return k2_iconv_open(iconv_cd, tocode, fromcode);
}

inline void iconv_close(void* iconv_cd) noexcept {
  k2_iconv_close(iconv_cd);
}

inline int32_t iconv(size_t* result, void* iconv_cd, char** inbuf, size_t* inbytesleft, char** outbuf, size_t* outbytesleft) noexcept {
  return k2_iconv(result, iconv_cd, inbuf, inbytesleft, outbuf, outbytesleft);
}

struct SymbolInfo {
  std::unique_ptr<char, decltype(std::addressof(k2::free))> name;
  std::unique_ptr<char, decltype(std::addressof(k2::free))> filename;
  uint32_t lineno;
};

inline std::expected<k2::SymbolInfo, int32_t> resolve_symbol(void* addr) noexcept {
  size_t name_len{};
  if (auto error_code{k2_symbol_name_len(addr, std::addressof(name_len))}; error_code != k2::errno_ok) [[unlikely]] {
    return std::unexpected{error_code};
  }
  size_t filename_len{};
  if (auto error_code{k2_symbol_filename_len(addr, std::addressof(filename_len))}; error_code != k2::errno_ok) [[unlikely]] {
    return std::unexpected{error_code};
  }

  // +1 since we get non-null-terminated strings from platform and we want to null-terminate them on our side
  auto* name{static_cast<char*>(k2::alloc(name_len + 1))};
  if (name == nullptr) [[unlikely]] {
    return std::unexpected{k2::errno_enomem};
  }

  auto* filename{static_cast<char*>(k2::alloc(filename_len + 1))};
  if (filename == nullptr) [[unlikely]] {
    return std::unexpected{k2::errno_enomem};
  }

  ::SymbolInfo symbol_info{.name = name, .filename = filename, .lineno = 0};
  if (auto error_code{k2_resolve_symbol(addr, std::addressof(symbol_info))}; error_code != k2::errno_ok) [[unlikely]] {
    k2::free(filename);
    k2::free(name);
    return std::unexpected{error_code};
  }

  // null-terminate
  name[name_len] = '\0';
  filename[filename_len] = '\0';

  return k2::SymbolInfo{.name = std::unique_ptr<char, decltype(std::addressof(k2::free))>{name, k2::free},
                        .filename = std::unique_ptr<char, decltype(std::addressof(k2::free))>{filename, k2::free},
                        .lineno = symbol_info.lineno};
}

inline int32_t code_segment_offset(uint64_t* offset) noexcept {
  return k2_code_segment_offset(offset);
}

inline size_t backtrace(std::span<void*> buffer) noexcept {
  return k2_backtrace(buffer.data(), buffer.size());
}

} // namespace k2

template<>
struct std::formatter<k2::SymbolInfo> {
  template<typename ParseContext>
  constexpr auto parse(ParseContext& ctx) const noexcept {
    return ctx.begin();
  }

  template<typename FmtContext>
  auto format(const k2::SymbolInfo& info, FmtContext& ctx) const noexcept {
    return std::format_to(ctx.out(), "{}\n\tat {}:{}", info.name.get(), info.filename.get(), info.lineno);
  }
};

inline constexpr bool operator<(const k2::TimePoint& lhs, const k2::TimePoint& rhs) noexcept {
  return lhs.time_point_ns < rhs.time_point_ns;
}
