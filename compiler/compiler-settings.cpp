// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "compiler/compiler-settings.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

#include "openssl/sha.h"

#include "common/algorithms/contains.h"
#include "common/algorithms/find.h"
#include "common/version-string.h"
#include "common/wrappers/fmt_format.h"
#include "common/wrappers/mkdir_recursive.h"
#include "common/wrappers/to_array.h"

#include "compiler/stage.h"
#include "compiler/threading/tls.h"
#include "compiler/utils/string-utils.h"

void KphpRawOption::init(const char *env, std::string default_value, std::vector<std::string> choices) noexcept {
  if (char *val = getenv(env)) {
    raw_option_arg_ = val;
  } else {
    raw_option_arg_ = std::move(default_value);
  }
  env_var_ = env;
  choices_ = std::move(choices);
}

void KphpRawOption::substitute_depends(const KphpRawOption &other) noexcept {
  raw_option_arg_ = vk::replace_all(raw_option_arg_, "${" + other.get_env_var() + "}", other.raw_option_arg_);
}

void KphpRawOption::verify_arg_value() const {
  if (!choices_.empty() && !vk::contains(choices_, raw_option_arg_)) {
    throw_param_exception("choose from " + vk::join(choices_, ", "));
  }
}

void KphpRawOption::throw_param_exception(const std::string &reason) const {
  throw std::runtime_error{"Can't parse " + get_env_var() + " option: " + reason};
}

template<>
void KphpOption<std::string>::dump_option(std::ostream &out) const noexcept {
  out << value_;
}

template<>
void KphpOption<uint64_t>::dump_option(std::ostream &out) const noexcept {
  out << value_;
}

template<>
void KphpOption<double>::dump_option(std::ostream &out) const noexcept {
  out << value_;
}

template<>
void KphpOption<bool>::dump_option(std::ostream &out) const noexcept {
  out << (value_ ? "true" : "false");
}

template<>
void KphpOption<std::vector<std::string>>::dump_option(std::ostream &out) const noexcept {
  out << vk::join(value_, ", ");
}

template<>
void KphpOption<std::string>::parse_arg_value() {
  // Don't move it, it can be used later
  value_ = raw_option_arg_;
}

template<>
void KphpOption<uint64_t>::parse_arg_value() {
  if (raw_option_arg_.empty()) {
    value_ = 0;
  } else {
    try {
      if (raw_option_arg_[0] == '-') {
        throw std::out_of_range{""};
      }
      value_ = std::stoul(raw_option_arg_);
    } catch (...) {
      throw_param_exception(fmt_format("unsigned integer is expected but {} found", raw_option_arg_));
    }
  }
}

template<>
void KphpOption<double>::parse_arg_value() {
  if (raw_option_arg_.empty()) {
    value_ = 0.0;
  } else {
    try {
      value_ = std::stod(raw_option_arg_);
    } catch (...) {
      throw_param_exception(fmt_format("double is expected but {} found", raw_option_arg_));
    }
  }
}

template<>
void KphpOption<bool>::parse_arg_value() {
  if (vk::none_of_equal(raw_option_arg_, "1", "0", "")) {
    throw_param_exception("'0' or '1' are expected");
  }
  value_ = raw_option_arg_ == "1";
}

template<>
void KphpOption<std::vector<std::string>>::parse_arg_value() {
  value_ = split(raw_option_arg_, ':');
}

namespace {

bool contains_lib(vk::string_view ld_flags, vk::string_view libname) noexcept {
  const std::string static_lib_name = "lib" + libname + ".a";
  if (vk::contains(ld_flags, static_lib_name)) {
    return true;
  }

  std::string shared_lib_name = "-l" + libname + " ";
  if (vk::contains(ld_flags, shared_lib_name)) {
    return true;
  }
  shared_lib_name.pop_back();
  return ld_flags.ends_with(shared_lib_name);
}

template<class T>
void append_if_doesnt_contain(std::string &ld_flags, const T &libs, vk::string_view prefix, vk::string_view suffix = {}) noexcept {
  for (vk::string_view lib : libs) {
    if (!contains_lib(ld_flags, lib)) {
      ld_flags.append(" ").append(prefix.begin(), prefix.end());
      ld_flags.append(lib.begin(), lib.end()).append(suffix.begin(), suffix.end());
    }
  }
}

void append_apple_options(std::string &cxx_flags, std::string &ld_flags) noexcept {
#if defined(__APPLE__)
#ifdef __arm64__
  std::string common_path = "/opt/homebrew";
#else
  std::string common_path = "/usr/local";
#endif
  cxx_flags += " -I" + common_path + "/include";
  ld_flags += " -liconv"
              " -lepoll-shim"
              " -L" EPOLL_SHIM_LIB_DIR
              " -L" + common_path + "/lib"
#ifdef PDO_DRIVER_PGSQL
              " -L" + common_path + "/opt/libpq/lib"
#endif
              " -undefined dynamic_lookup";

#else
  static_cast<void>(cxx_flags);
  static_cast<void>(ld_flags);
#endif
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

std::string calc_cxx_flags_sha256(vk::string_view cxx, vk::string_view cxx_flags_line) noexcept {
  SHA256_CTX sha256;
  SHA256_Init(&sha256);

  SHA256_Update(&sha256, cxx.data(), cxx.size());
  SHA256_Update(&sha256, cxx_flags_line.data(), cxx_flags_line.size());

  unsigned char hash[SHA256_DIGEST_LENGTH] = {0};
  SHA256_Final(hash, &sha256);

  std::string hash_str;
  hash_str.reserve(SHA256_DIGEST_LENGTH * 2);
  for (auto hash_symb : hash) {
    fmt_format_to(std::back_inserter(hash_str), "{:02x}", hash_symb);
  }
  return hash_str;
}

#pragma GCC diagnostic pop

} // namespace

void CxxFlags::init(const std::string &runtime_sha256, const std::string &cxx,
                    std::string cxx_flags_line, const std::string &dest_cpp_dir, bool enable_pch) noexcept {
  remove_extra_spaces(cxx_flags_line);
  flags.value_ = std::move(cxx_flags_line);
  flags_sha256.value_ = calc_cxx_flags_sha256(cxx, flags.get());
  flags.value_.append(" -iquote").append(dest_cpp_dir);
  if (enable_pch) {
    pch_dir.value_.append("/tmp/kphp_pch/").append(runtime_sha256).append("/").append(flags_sha256.get()).append("/");
  }
}

void CompilerSettings::option_as_dir(KphpOption<std::string> &path_option) noexcept {
  path_option.value_ = as_dir(path_option.value_);
}

bool CompilerSettings::is_composer_enabled() const {
  return !composer_root.get().empty();
}

std::string CompilerSettings::get_version() const {
  return override_kphp_version.get().empty() ? get_version_string() : override_kphp_version.get();
}

void CompilerSettings::init() {
  option_as_dir(kphp_src_path);
  functions_file.value_ = get_full_path(functions_file.get());
  runtime_sha256_file.value_ = get_full_path(runtime_sha256_file.get());
  const auto now{std::chrono::system_clock::now()};
  build_timestamp.value_ = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());

  bool is_k2_mode = mode.get().substr(0, 3) == "k2-";
  if (link_file.value_.empty()) {
    if (is_k2_mode) {
      link_file.value_ = kphp_src_path.get() + "/objs/libk2kphp-rt.a";
    } else {
      if (dynamic_incremental_linkage.get()) {
        link_file.value_ = kphp_src_path.get() + "/objs/libkphp-full-runtime-pic.a";
      } else {
        link_file.value_ = kphp_src_path.get() + "/objs/libkphp-full-runtime-no-pic.a";
      }
    }
  }
  link_file.value_ = get_full_path(link_file.get());
  if (functions_file.value_.empty()) {
    if (is_k2_mode) {
      functions_file.value_ = kphp_src_path.get() + "/builtin-functions/kphp-light/functions.txt";
    } else {
      functions_file.value_ = kphp_src_path.get() + "/builtin-functions/kphp-full/_functions.txt";
    }
  }
  functions_file.value_ = get_full_path(functions_file.get());

  if (k2_component_name.get() != "KPHP") {
    kphp_error(is_k2_mode, "Option \"k2-component-name\" is only available for k2 component modes");
  }

  if (mode.get() == "lib") {
    if (!tl_schema_file.get().empty()) {
      throw std::runtime_error{"Option " + tl_schema_file.get_env_var() + " is forbidden for static lib mode"};
    }
    std::string lib_dir = get_full_path(main_file.get());
    std::size_t last_slash = lib_dir.rfind('/');
    if (last_slash == std::string::npos) {
      throw std::runtime_error{"Bad lib directory"};
    }
    static_lib_name.value_ = lib_dir.substr(last_slash + 1);
    if (static_lib_name.get().empty()) {
      throw std::runtime_error{"Empty static lib name"};
    }
    lib_dir = as_dir(lib_dir);
    includes.value_.emplace_back(lib_dir + "php/");
    if (static_lib_out_dir.get().empty()) {
      static_lib_out_dir.value_ = lib_dir + "lib/";
    }

    option_as_dir(static_lib_out_dir);
    main_file.value_.assign(lib_dir).append("/php/index.php");
  } else if (!static_lib_out_dir.get().empty()) {
    throw std::runtime_error{"Option " + static_lib_out_dir.get_env_var() + " is allowed only for static lib mode"};
  }

  if (!jobs_count.get()) {
    jobs_count.value_ = get_default_threads_count();
  }
  if (!threads_count.get()) {
    threads_count.value_ = get_default_threads_count();
  } else if (threads_count.get() > MAX_THREADS_COUNT) {
    throw std::runtime_error{"Option " + threads_count.get_env_var() + " is expected to be <= " + std::to_string(MAX_THREADS_COUNT)};
  }

  for (std::string &include : includes.value_) {
    include = as_dir(include);
  }

  if (colorize.get() == "auto") {
    color_ = auto_colored;
  } else if (colorize.get() == "no") {
    color_ = not_colored;
  } else if (colorize.get() == "yes") {
    color_ = colored;
  } else {
    kphp_assert(0);
  }

  remove_extra_spaces(extra_cxx_flags.value_);
  std::stringstream ss;
  ss << "-Wall "
     << extra_cxx_flags.get()
     << " -iquote" << kphp_src_path.get()
     << " -iquote " << kphp_src_path.get()
     << "objs/generated/auto/runtime"
     << " -fwrapv -Wno-parentheses -Wno-trigraphs"
     << " -fno-strict-aliasing -fno-omit-frame-pointer";
#ifdef __x86_64__
  ss << " -march=sandybridge";
#elif __aarch64__
  ss << " -march=armv8.2-a+crypto";
#endif
  if (!no_pch.get()) {
    ss << " -Winvalid-pch -fpch-preprocess";
  }
  if (is_k2_mode || dynamic_incremental_linkage.get()) {
    ss << " -fPIC";
  }
  if (vk::contains(cxx.get(), "clang")) {
    ss << " -Wno-invalid-source-encoding";
  }
  #if __cplusplus <= 201703L
    ss << " -std=c++17";
  #elif __cplusplus <= 202002L
    ss << " -std=c++20";
  #elif __cplusplus <= 202302L
    ss << " -std=c++23";
  #else
    #error unsupported __cplusplus value
  #endif

  ss << " -I" << kphp_src_path.get() + "objs/include ";
  if (is_k2_mode) {
    // for now k2-component must be compiled with clang and statically linked libc++
    ss << " -stdlib=libc++";
  } else {
    // default value is false
    // when we build using full runtime, we should force to use runtime as static lib
    force_link_runtime.value_ = true;
  }
  std::string cxx_default_flags = ss.str();

  cxx_toolchain_option.value_ = !cxx_toolchain_dir.value_.empty() ? ("-B" + cxx_toolchain_dir.value_) : "";

  if (dynamic_incremental_linkage.get()) {
    incremental_linker_flags.value_ = "-shared";
    if (is_k2_mode) {
      incremental_linker_flags.value_ += " -stdlib=libc++ -static-libstdc++";
    }
  } else {
    incremental_linker_flags.value_ = "-r -nostdlib";
  }

  remove_extra_spaces(extra_ld_flags.value_);

  ld_flags.value_ = extra_ld_flags.get();
  append_apple_options(cxx_default_flags, ld_flags.value_);
  std::vector<vk::string_view> system_installed_static_libs{};
  std::vector<vk::string_view> system_installed_dynamic_libs{"pthread", "m", "dl"};

#ifdef PDO_DRIVER_MYSQL
#ifdef PDO_LIBS_STATIC_LINKING
  system_installed_static_libs.emplace_back("mysqlclient");
#else
  system_installed_dynamic_libs.emplace_back("mysqlclient");
#endif
#endif

#ifdef PDO_DRIVER_PGSQL
#ifdef PDO_LIBS_STATIC_LINKING
  ld_flags.value_ += fmt_format(" -L /usr/lib/postgresql/{}/lib/ ", PDO_DRIVER_PGSQL_VERSION);
  system_installed_static_libs.emplace_back("pq");
  system_installed_static_libs.emplace_back("pgcommon");
  system_installed_static_libs.emplace_back("pgport");
  // following common libraries are required for libpq.a
  system_installed_dynamic_libs.emplace_back("ldap");
  system_installed_dynamic_libs.emplace_back("gssapi_krb5");
#else
  system_installed_dynamic_libs.emplace_back("pq");
#endif
#endif

#if defined(__APPLE__)
  append_if_doesnt_contain(ld_flags.value_, system_installed_static_libs, "-l");
  auto flex_prefix = kphp_src_path.value_ + "objs/flex/lib";
  append_if_doesnt_contain(ld_flags.value_, vk::to_array({"vk-flex-data"}), flex_prefix, ".a");
  system_installed_dynamic_libs.emplace_back("iconv");
#else
  system_installed_static_libs.emplace_back("vk-flex-data");
  append_if_doesnt_contain(ld_flags.value_, system_installed_static_libs, "-l:lib", ".a");
  system_installed_dynamic_libs.emplace_back("rt");
#endif

  append_if_doesnt_contain(ld_flags.value_, system_installed_dynamic_libs, "-l");
  ld_flags.value_ += " -rdynamic";

  runtime_headers.value_ = "runtime-headers.h";
  runtime_sha256.value_ = read_runtime_sha256_file(runtime_sha256_file.get());

  auto full_path = get_full_path(main_file.get());
  if (full_path.empty()) {
    // get_full_path() calls realpath() which will set the errno in case of failure
    throw std::runtime_error{fmt_format("Failed to open file [{}] : {}", main_file.get(), strerror(errno))};
  }
  main_file.value_.assign(full_path);

  const size_t dir_pos = main_file.get().rfind('/');
  kphp_assert (dir_pos != std::string::npos);
  base_dir.value_ = main_file.get().substr(0, dir_pos + 1);

  mkdir_recursive(dest_dir.get().c_str(), 0777);
  option_as_dir(dest_dir);
  dest_cpp_dir.value_ = dest_dir.get() + "kphp/";
  dest_objs_dir.value_ = dest_dir.get() + "objs/";
  if (is_k2_mode) {
    binary_path.value_ = dest_dir.get() + "k2." + k2_component_name.get() + "." + build_timestamp.value_ + ".so";
  } else {
    binary_path.value_ = dest_dir.get() + mode.get();
  }
  performance_analyze_report_path.value_ = dest_dir.get() + "performance_issues.json";
  generated_runtime_path.value_ = kphp_src_path.get() + "objs/generated/auto/runtime/";

  cxx_flags_default.init(runtime_sha256.value_, cxx.get(), cxx_default_flags, dest_cpp_dir.get(), !no_pch.get());
  cxx_default_flags.append(" ").append(extra_cxx_debug_level.get());
  cxx_flags_with_debug.init(runtime_sha256.value_, cxx.get(), cxx_default_flags, dest_cpp_dir.get(), !no_pch.get());

  tl_namespace_prefix.value_ = "VK\\TL\\";
  tl_classname_prefix.value_ = "C$VK$TL$";

  option_as_dir(composer_root);
}

std::string CompilerSettings::read_runtime_sha256_file(const std::string &filename) {
  std::ifstream runtime_sha256_file(filename.c_str());
  kphp_error(runtime_sha256_file,
             fmt_format("Can't open runtime sha256 file '{}'", filename));

  constexpr std::streamsize SHA256_LEN = 64;
  char runtime_sha256[SHA256_LEN] = {0};
  runtime_sha256_file.read(runtime_sha256, SHA256_LEN);
  kphp_error(runtime_sha256_file.gcount() == SHA256_LEN,
             fmt_format("Can't read runtime sha256 from file '{}'", filename));
  return {runtime_sha256, runtime_sha256 + SHA256_LEN};
}

CompilerSettings::color_settings CompilerSettings::get_color_settings() const {
  return color_;
}
