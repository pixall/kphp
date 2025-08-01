#pragma once

#include <cstdlib>

#include "common/wrappers/likely.h"

// For reporting php logic related errors happened during script execution
void php_debug(char const* message, ...) __attribute__((format(printf, 1, 2)));
void php_notice(char const* message, ...) __attribute__((format(printf, 1, 2)));
void php_warning(char const* message, ...) __attribute__((format(printf, 1, 2)));
void php_error(char const* message, ...) __attribute__((format(printf, 1, 2)));

// For reporting internal runtime errors happened during script execution
// void runtime_debug(char const* message, ...) __attribute__((format(printf, 1, 2)));
// void runtime_notice(char const* message, ...) __attribute__((format(printf, 1, 2)));
// void runtime_warning(char const* message, ...) __attribute__((format(printf, 1, 2)));
void runtime_error(char const* message, ...) __attribute__((format(printf, 1, 2)));

[[noreturn]] void php_assert__(const char* msg, const char* file, int line);
[[noreturn]] void critical_error_handler();

#define php_assert(EX)                                                                                                                                         \
  do {                                                                                                                                                         \
    if (unlikely(!(EX))) {                                                                                                                                     \
      php_assert__(#EX, __FILE__, __LINE__);                                                                                                                   \
    }                                                                                                                                                          \
  } while (0)

#define php_critical_error(format, ...)                                                                                                                        \
  do {                                                                                                                                                         \
    php_error("Critical error \"" format "\" in file %s on line %d", ##__VA_ARGS__, __FILE__, __LINE__);                                                       \
    critical_error_handler();                                                                                                                                  \
  } while (0)

#define runtime_critical_error(format, ...)                                                                                                                    \
  do {                                                                                                                                                         \
    runtime_error("Critical error \"" format "\" in file %s on line %d", ##__VA_ARGS__, __FILE__, __LINE__);                                                   \
    critical_error_handler();                                                                                                                                  \
  } while (0)
