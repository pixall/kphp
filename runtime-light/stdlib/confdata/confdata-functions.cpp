// Compiler for PHP (aka KPHP)
// Copyright (c) 2024 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "runtime-light/stdlib/confdata/confdata-functions.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

#include "runtime-common/core/runtime-core.h"
#include "runtime-common/stdlib/serialization/json-functions.h"
#include "runtime-common/stdlib/serialization/serialize-functions.h"
#include "runtime-light/coroutine/task.h"
#include "runtime-light/stdlib/component/component-api.h"
#include "runtime-light/stdlib/confdata/confdata-constants.h"
#include "runtime-light/tl/tl-core.h"
#include "runtime-light/tl/tl-functions.h"
#include "runtime-light/tl/tl-types.h"
#include "runtime-light/utils/logs.h"

namespace {

mixed extract_confdata_value(const tl::confdataValue& confdata_value) noexcept {
  if (confdata_value.is_php_serialized.value && confdata_value.is_json_serialized.value) [[unlikely]] { // check that we don't have both flags set
    kphp::log::warning("confdata value has both php_serialized and json_serialized flags set");
    return {};
  }
  if (confdata_value.is_php_serialized.value) {
    return f$unserialize(string{confdata_value.value.value.data(), static_cast<string::size_type>(confdata_value.value.value.size())});
  } else if (confdata_value.is_json_serialized.value) {
    return f$json_decode(string{confdata_value.value.value.data(), static_cast<string::size_type>(confdata_value.value.value.size())});
  } else {
    return string{confdata_value.value.value.data(), static_cast<string::size_type>(confdata_value.value.value.size())};
  }
}

} // namespace

kphp::coro::task<mixed> f$confdata_get_value(string key) noexcept {
  tl::TLBuffer tlb{};
  tl::ConfdataGet{.key = {.value = {key.c_str(), key.size()}}}.store(tlb);

  auto query{
      co_await f$component_client_send_request({kphp::confdata::COMPONENT_NAME.data(), static_cast<string::size_type>(kphp::confdata::COMPONENT_NAME.size())},
                                               {tlb.data(), static_cast<string::size_type>(tlb.size())})};
  if (query.is_null()) [[unlikely]] {
    co_return mixed{};
  }
  const auto response{co_await f$component_client_fetch_response(std::move(query))};

  tlb.clean();
  tlb.store_bytes({response.c_str(), static_cast<size_t>(response.size())});
  tl::Maybe<tl::confdataValue> maybe_confdata_value{};
  kphp::log::assertion(maybe_confdata_value.fetch(tlb));

  if (!maybe_confdata_value.opt_value) { // no such key
    co_return mixed{};
  }
  co_return extract_confdata_value(*maybe_confdata_value.opt_value); // the key exists
}

kphp::coro::task<array<mixed>> f$confdata_get_values_by_any_wildcard(string wildcard) noexcept {
  tl::TLBuffer tlb{};
  tl::ConfdataGetWildcard{.wildcard = {.value = {wildcard.c_str(), wildcard.size()}}}.store(tlb);

  auto query{
      co_await f$component_client_send_request({kphp::confdata::COMPONENT_NAME.data(), static_cast<string::size_type>(kphp::confdata::COMPONENT_NAME.size())},
                                               {tlb.data(), static_cast<string::size_type>(tlb.size())})};
  if (query.is_null()) [[unlikely]] {
    co_return array<mixed>{};
  }
  const auto response{co_await f$component_client_fetch_response(std::move(query))};

  tlb.clean();
  tlb.store_bytes({response.c_str(), static_cast<size_t>(response.size())});
  tl::Dictionary<tl::confdataValue> dict_confdata_value{};
  kphp::log::assertion(dict_confdata_value.fetch(tlb));

  array<mixed> result{array_size{static_cast<int64_t>(dict_confdata_value.size()), false}};
  std::for_each(dict_confdata_value.begin(), dict_confdata_value.end(), [&result](const auto& dict_field) noexcept {
    result.set_value(string{dict_field.key.value.data(), static_cast<string::size_type>(dict_field.key.value.size())},
                     extract_confdata_value(dict_field.value));
  });
  co_return std::move(result);
}
