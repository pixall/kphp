// Compiler for PHP (aka KPHP)
// Copyright (c) 2022 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#pragma once

#include <cstdint>
#include <string_view>

#include "common/algorithms/hashes.h"
#include "runtime-common/core/runtime-core.h"
#include "runtime-common/stdlib/visitors/dummy-visitor-methods.h"
#include "runtime/datetime/datetime_interface.h"
#include "runtime/datetime/datetime_zone.h"

struct C$DateInterval;
struct C$DateTime;

struct C$DateTimeImmutable : public C$DateTimeInterface, private DummyVisitorMethods {
  using DummyVisitorMethods::accept;

  const char* get_class() const noexcept override {
    return R"(DateTimeImmutable)";
  }

  int32_t get_hash() const noexcept override {
    std::string_view name_view{C$DateTimeImmutable::get_class()};
    return static_cast<int32_t>(vk::murmur_hash<uint32_t>(name_view.data(), name_view.size()));
  }

  ~C$DateTimeImmutable() override;
};

extern const string NOW;
class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$__construct(const class_instance<C$DateTimeImmutable>& self, const string& datetime = NOW,
                                                                     const class_instance<C$DateTimeZone>& timezone = Optional<bool>{}) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$add(const class_instance<C$DateTimeImmutable>& self,
                                                             const class_instance<C$DateInterval>& interval) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$createFromFormat(const string& format, const string& datetime,
                                                                          const class_instance<C$DateTimeZone>& timezone = Optional<bool>{}) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$createFromMutable(const class_instance<C$DateTime>& object) noexcept;

Optional<array<mixed>> f$DateTimeImmutable$$getLastErrors() noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$modify(const class_instance<C$DateTimeImmutable>& self, const string& modifier) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$setDate(const class_instance<C$DateTimeImmutable>& self, int64_t year, int64_t month,
                                                                 int64_t day) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$setISODate(const class_instance<C$DateTimeImmutable>& self, int64_t year, int64_t week,
                                                                    int64_t dayOfWeek = 1) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$setTime(const class_instance<C$DateTimeImmutable>& self, int64_t hour, int64_t minute,
                                                                 int64_t second = 0, int64_t microsecond = 0) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$setTimestamp(const class_instance<C$DateTimeImmutable>& self, int64_t timestamp) noexcept;

class_instance<C$DateTimeImmutable> f$DateTimeImmutable$$sub(const class_instance<C$DateTimeImmutable>& self,
                                                             const class_instance<C$DateInterval>& interval) noexcept;

class_instance<C$DateInterval> f$DateTimeImmutable$$diff(const class_instance<C$DateTimeImmutable>& self,
                                                         const class_instance<C$DateTimeInterface>& target_object, bool absolute = false) noexcept;

string f$DateTimeImmutable$$format(const class_instance<C$DateTimeImmutable>& self, const string& format) noexcept;

int64_t f$DateTimeImmutable$$getOffset(const class_instance<C$DateTimeImmutable>& self) noexcept;

int64_t f$DateTimeImmutable$$getTimestamp(const class_instance<C$DateTimeImmutable>& self) noexcept;
