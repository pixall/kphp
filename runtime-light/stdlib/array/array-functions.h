// Compiler for PHP (aka KPHP)
// Copyright (c) 2024 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "runtime-common/core/runtime-core.h"
#include "runtime-light/coroutine/task.h"
#include "runtime-light/coroutine/type-traits.h"
#include "runtime-light/stdlib/diagnostics/logs.h"
#include "runtime-light/stdlib/math/random-functions.h"

namespace array_functions_impl_ {
template<typename T, typename Comparator>
requires(std::invocable<Comparator, T, T> && kphp::coro::is_async_function_v<Comparator, T, T>)
kphp::coro::task<> async_sort(T* begin_init, T* end_init, Comparator compare) noexcept {
  T* begin_stack[32];
  T* end_stack[32];

  begin_stack[0] = begin_init;
  end_stack[0] = end_init - 1;

  for (int depth = 0; depth >= 0; --depth) {
    T* begin = begin_stack[depth];
    T* end = end_stack[depth];

    while (begin < end) {
      const auto offset = (end - begin) >> 1;
      swap(*begin, begin[offset]);

      T* i = begin + 1;
      T* j = end;

      while (true) {
        while (i < j && (co_await std::invoke(compare, *begin, *i)) > 0) {
          i++;
        }

        while (i <= j && (co_await std::invoke(compare, *j, *begin)) > 0) {
          j--;
        }

        if (i >= j) {
          break;
        }

        swap(*i++, *j--);
      }

      swap(*begin, *j);

      if (j - begin <= end - j) {
        if (j + 1 < end) {
          begin_stack[depth] = j + 1;
          end_stack[depth++] = end;
        }
        end = j - 1;
      } else {
        if (begin < j - 1) {
          begin_stack[depth] = begin;
          end_stack[depth++] = j - 1;
        }
        begin = j + 1;
      }
    }
  }
  co_return;
}

template<typename Result, typename U, typename Comparator>
Result async_sort(array<U>& arr, Comparator comparator, bool renumber) noexcept {
  using array_inner = typename array<U>::array_inner;
  using array_bucket = typename array<U>::array_bucket;
  int64_t n = arr.count();

  if (renumber) {
    if (n == 0) {
      co_return;
    }

    if (!arr.is_vector()) {
      array_inner* res = array_inner::create(n, true);
      for (array_bucket* it = arr.p->begin(); it != arr.p->end(); it = arr.p->next(it)) {
        res->push_back_vector_value(it->value);
      }

      arr.p->dispose();
      arr.p = res;
    } else {
      arr.mutate_if_vector_shared();
    }

    U* begin = reinterpret_cast<U*>(arr.p->entries());
    co_await async_sort<U, decltype(comparator)>(begin, begin + n, std::move(comparator));
    co_return;
  }

  if (n <= 1) {
    co_return;
  }

  if (arr.is_vector()) {
    arr.convert_to_map();
  } else {
    arr.mutate_if_map_shared();
  }
  auto& runtimeAllocator{RuntimeAllocator::get()};

  auto** arTmp = static_cast<array_bucket**>(runtimeAllocator.alloc_script_memory(n * sizeof(array_bucket*)));
  uint32_t i = 0;
  for (array_bucket* it = arr.p->begin(); it != arr.p->end(); it = arr.p->next(it)) {
    arTmp[i++] = it;
  }
  kphp::log::assertion(i == n);

  const auto hash_entry_cmp = []<typename Compare>(Compare compare, const array_bucket* lhs, const array_bucket* rhs) -> kphp::coro::task<bool> {
    co_return (co_await std::invoke(compare, lhs->value, rhs->value)) > 0;
  };

  const auto partial_hash_entry_cmp = std::bind_front(hash_entry_cmp, std::move(comparator));

  co_await async_sort<array_bucket*, decltype(partial_hash_entry_cmp)>(arTmp, arTmp + n, partial_hash_entry_cmp);

  arTmp[0]->prev = arr.p->get_pointer(arr.p->end());
  arr.p->end()->next = arr.p->get_pointer(arTmp[0]);
  for (uint32_t j = 1; j < n; j++) {
    arTmp[j]->prev = arr.p->get_pointer(arTmp[j - 1]);
    arTmp[j - 1]->next = arr.p->get_pointer(arTmp[j]);
  }
  arTmp[n - 1]->next = arr.p->get_pointer(arr.p->end());
  arr.p->end()->prev = arr.p->get_pointer(arTmp[n - 1]);

  runtimeAllocator.free_script_memory(arTmp, n * sizeof(array_bucket*));
}

template<typename Result, typename U, typename Comparator>
Result async_ksort(array<U>& arr, Comparator comparator) noexcept {
  using array_bucket = typename array<U>::array_bucket;
  using key_type = typename array<U>::key_type;
  using list_hash_entry = typename array<U>::list_hash_entry;

  int64_t n = arr.count();
  if (n <= 1) {
    co_return;
  }

  if (arr.is_vector()) {
    arr.convert_to_map();
  } else {
    arr.mutate_if_map_shared();
  }

  array<key_type> keys(array_size(n, true));
  for (auto* it = arr.p->begin(); it != arr.p->end(); it = arr.p->next(it)) {
    keys.p->push_back_vector_value(it->get_key());
  }

  auto* keysp = reinterpret_cast<key_type*>(keys.p->entries());
  co_await async_sort<key_type, Comparator>(keysp, keysp + n, std::move(comparator));

  auto* prev = static_cast<list_hash_entry*>(arr.p->end());
  for (uint32_t j = 0; j < n; j++) {
    list_hash_entry* cur = nullptr;
    if (arr.is_int_key(keysp[j])) {
      int64_t int_key = keysp[j].to_int();
      uint32_t bucket = arr.p->choose_bucket(int_key);
      while (arr.p->entries()[bucket].int_key != int_key || !arr.p->entries()[bucket].string_key.is_dummy_string()) {
        if (++bucket == arr.p->buf_size) [[unlikely]] {
          bucket = 0;
        }
      }
      cur = static_cast<list_hash_entry*>(&arr.p->entries()[bucket]);
    } else {
      string string_key = keysp[j].to_string();
      int64_t int_key = string_key.hash();
      array_bucket* string_entries = arr.p->entries();
      uint32_t bucket = arr.p->choose_bucket(int_key);
      while ((string_entries[bucket].int_key != int_key || string_entries[bucket].string_key.is_dummy_string() ||
              string_entries[bucket].string_key != string_key)) {
        if (++bucket == arr.p->buf_size) [[unlikely]] {
          bucket = 0;
        }
      }
      cur = static_cast<list_hash_entry*>(&string_entries[bucket]);
    }

    cur->prev = arr.p->get_pointer(prev);
    prev->next = arr.p->get_pointer(cur);

    prev = cur;
  }
  prev->next = arr.p->get_pointer(arr.p->end());
  arr.p->end()->prev = arr.p->get_pointer(prev);
}

template<typename T>
concept convertible_to_php_bool = requires(T t) {
  { f$boolval(t) } -> std::convertible_to<bool>;
};

template<class T, std::invocable<T> F>
requires convertible_to_php_bool<kphp::coro::async_function_return_type_t<F, T>>
kphp::coro::task<array<T>> array_filter_impl(array<T> a, F f) noexcept {
  array<T> result{a.size()};
  for (const auto& it : a) {
    bool condition{};
    if constexpr (kphp::coro::is_async_function_v<F, T>) {
      condition = f$boolval(co_await std::invoke(f, it.get_value()));
    } else {
      condition = f$boolval(std::invoke(f, it.get_value()));
    }
    if (condition) {
      result.set_value(it);
    }
  }
  co_return std::move(result);
}

template<class T, std::invocable<typename array<T>::const_iterator::key_type> F>
requires convertible_to_php_bool<kphp::coro::async_function_return_type_t<F, typename array<T>::const_iterator::key_type>>
kphp::coro::task<array<T>> array_filter_by_key_impl(array<T> a, F f) noexcept {
  array<T> result{a.size()};
  for (const auto& it : a) {
    bool condition{};
    if constexpr (kphp::coro::is_async_function_v<F, typename array<T>::const_iterator::key_type>) {
      condition = f$boolval(co_await std::invoke(f, it.get_key()));
    } else {
      condition = f$boolval(std::invoke(f, it.get_key()));
    }
    if (condition) {
      result.set_value(it);
    }
  }
  co_return std::move(result);
}

template<class T, class F>
requires convertible_to_php_bool<kphp::coro::async_function_return_type_t<F, typename array<T>::const_iterator::value_type>>
kphp::coro::task<std::tuple<typename array<T>::const_iterator::key_type, typename array<T>::const_iterator::value_type>> array_find_impl(array<T> a,
                                                                                                                                         F f) noexcept {
  for (const auto& it : a) {
    bool condition{};
    if constexpr (kphp::coro::is_async_function_v<F, typename array<T>::const_iterator::value_type>) {
      condition = co_await std::invoke(f, it.get_value());
    } else {
      condition = std::invoke(f, it.get_value());
    }
    if (condition) {
      co_return std::tuple{it.get_key(), it.get_value()};
    }
  }
  co_return std::tuple<typename array<T>::const_iterator::key_type, typename array<T>::const_iterator::value_type>{};
}

} // namespace array_functions_impl_

template<class T>
void f$shuffle(array<T>& arr) noexcept {
  const auto arr_size{arr.count()};
  if (arr_size <= 1) [[unlikely]] {
    return;
  }

  array<T> shuffled{array_size(arr_size, true)};
  for (const auto& it : arr) {
    shuffled.push_back(it.get_value());
  }
  for (int64_t i = 1; i < arr_size; i++) {
    swap(shuffled[i], shuffled[f$mt_rand(0, i)]);
  }

  arr = std::move(shuffled);
}

template<class T>
kphp::coro::task<array<T>> f$array_filter(array<T> a) noexcept {
  co_return co_await array_functions_impl_::array_filter_impl(std::move(a), std::identity{});
}

template<class T, std::invocable<T> F>
kphp::coro::task<array<T>> f$array_filter(array<T> a, F f) noexcept {
  co_return co_await array_functions_impl_::array_filter_impl(std::move(a), std::move(f));
}

template<class T, std::invocable<typename array<T>::const_iterator::key_type> F>
kphp::coro::task<array<T>> f$array_filter_by_key(array<T> a, F f) noexcept {
  co_return co_await array_functions_impl_::array_filter_by_key_impl(std::move(a), std::move(f));
}

template<class T, std::invocable<T> F>
kphp::coro::task<std::tuple<typename array<T>::const_iterator::key_type, typename array<T>::const_iterator::value_type>> f$array_find(array<T> a, F f) {
  co_return co_await array_functions_impl_::array_find_impl(std::move(a), std::move(f));
}

template<class T>
typename array<T>::key_type f$array_rand(const array<T>& a) noexcept {
  if (int64_t size{a.count()}) {
    return a.middle(f$mt_rand(0, size - 1)).get_key();
  }
  return {};
}

template<class T>
mixed f$array_rand(const array<T>& a, int64_t num) noexcept {
  if (num == 1) {
    return f$array_rand(a);
  }

  int64_t size{a.count()};

  if (num <= 0 || num > size) [[unlikely]] {
    kphp::log::warning("second argument has to be between 1 and the number of elements in the array");
    return {};
  }

  array<typename array<T>::key_type> result{array_size(num, true)};
  for (const auto& it : a) {
    if (f$mt_rand(0, --size) < num) {
      result.push_back(it.get_key());
      --num;
    }
  }

  return result;
}

/**
 * Currently, array_map is always considered async. Despite we rely on symmetric transfer optimization,
 * we need to be careful with such functions. We may want to split such functions into sync and async
 * versions in case we face with performance problems.
 */
template<class A, std::invocable<A> F, class R = kphp::coro::async_function_return_type_t<F, A>>
kphp::coro::task<array<R>> f$array_map(F f, array<A> a) noexcept {
  array<R> result{a.size()};
  for (const auto& it : a) {
    if constexpr (kphp::coro::is_async_function_v<F, A>) {
      result.set_value(it.get_key(), co_await std::invoke(f, it.get_value()));
    } else {
      result.set_value(it.get_key(), std::invoke(f, it.get_value()));
    }
  }
  co_return std::move(result);
}

template<class R, class T, std::invocable<R, T> F, class I>
requires std::constructible_from<R, std::add_rvalue_reference_t<I>>
kphp::coro::task<R> f$array_reduce(array<T> a, F f, I init) noexcept {
  R result{R(std::move(init))}; // explicit constructor call to avoid implicit cast
  for (const auto& it : a) {
    if constexpr (kphp::coro::is_async_function_v<F, R, T>) {
      result = co_await std::invoke(f, result, it.get_value());
    } else {
      result = std::invoke(f, result, it.get_value());
    }
  }
  co_return std::move(result);
}

template<class T, class Comparator>
requires(std::invocable<Comparator, T, T>)
kphp::coro::task<> f$usort(array<T>& a, Comparator compare) {
  if constexpr (kphp::coro::is_async_function_v<Comparator, T, T>) {
    /* ATTENTION: temporary copy is necessary since functions is coroutine and sort is inplace */
    array<T> tmp{a};
    co_await array_functions_impl_::async_sort<kphp::coro::task<>>(tmp, std::move(compare), true);
    a = tmp;
    co_return;
  } else {
    co_return a.sort(std::move(compare), true);
  }
}

template<class T, class Comparator>
requires(std::invocable<Comparator, T, T>)
kphp::coro::task<> f$uasort(array<T>& a, Comparator compare) {
  if constexpr (kphp::coro::is_async_function_v<Comparator, T, T>) {
    /* ATTENTION: temporary copy is necessary since functions is coroutine and sort is inplace */
    array<T> tmp{a};
    co_await array_functions_impl_::async_sort<kphp::coro::task<>>(tmp, std::move(compare), false);
    a = tmp;
  } else {
    co_return a.sort(std::move(compare), false);
  }
}

template<class T, class Comparator>
requires(std::invocable<Comparator, typename array<T>::key_type, typename array<T>::key_type>)
kphp::coro::task<> f$uksort(array<T>& a, Comparator compare) {
  if constexpr (kphp::coro::is_async_function_v<Comparator, typename array<T>::key_type, typename array<T>::key_type>) {
    /* ATTENTION: temporary copy is necessary since functions is coroutine and sort is inplace */
    array<T> tmp{a};
    co_await array_functions_impl_::async_ksort<kphp::coro::task<>>(tmp, std::move(compare));
    a = tmp;
  } else {
    co_return a.ksort(std::move(compare));
  }
}

template<class T>
T f$vk_dot_product(const array<T>& /*unused*/, const array<T>& /*unused*/) {
  kphp::log::error("call to unsupported function");
}
