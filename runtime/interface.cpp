// Compiler for PHP (aka KPHP)
// Copyright (c) 2020 LLC «V Kontakte»
// Distributed under the GPL v3 License, see LICENSE.notice.txt

#include "runtime/interface.h"

#include <arpa/inet.h>
#include <cassert>
#include <clocale>
#include <csetjmp>
#include <csignal>
#include <fstream>
#include <functional>
#include <getopt.h>
#include <netdb.h>
#include <unistd.h>

#include "common/algorithms/string-algorithms.h"
#include "common/macos-ports.h"
#include "common/tl/constants/common.h"
#include "common/wrappers/overloaded.h"

#include "net/net-connections.h"
#include "runtime-common/stdlib/serialization/serialization-context.h"
#include "runtime-common/stdlib/server/url-functions.h"
#include "runtime-common/stdlib/string/string-context.h"
#include "runtime-common/stdlib/string/string-functions.h"
#include "runtime-common/stdlib/tracing/tracing-functions.h"
#include "runtime/array_functions.h"
#include "runtime/bcmath.h"
#include "runtime/confdata-functions.h"
#include "runtime/context/runtime-context.h"
#include "runtime/critical_section.h"
#include "runtime/curl.h"
#include "runtime/datetime/datetime_functions.h"
#include "runtime/datetime/timelib_wrapper.h"
#include "runtime/exception.h"
#include "runtime/files.h"
#include "runtime/instance-cache.h"
#include "runtime/job-workers/client-functions.h"
#include "runtime/job-workers/server-functions.h"
#include "runtime/kphp-backtrace.h"
#include "runtime/kphp_ml/kphp_ml_init.h"
#include "runtime/kphp_tracing.h"
#include "runtime/math_functions.h"
#include "runtime/memcache.h"
#include "runtime/mysql.h"
#include "runtime/net_events.h"
#include "runtime/on_kphp_warning_callback.h"
#include "runtime/oom_handler.h"
#include "runtime/openssl.h"
#include "runtime/pdo/pdo.h"
#include "runtime/profiler.h"
#include "runtime/regexp.h"
#include "runtime/resumable.h"
#include "runtime/rpc.h"
#include "runtime/runtime-builtin-stats.h"
#include "runtime/streams.h"
#include "runtime/tcp.h"
#include "runtime/typed_rpc.h"
#include "runtime/udp.h"
#include "runtime/url.h"
#include "runtime/zlib.h"
#include "server/curl-adaptor.h"
#include "server/database-drivers/adaptor.h"
#include "server/database-drivers/mysql/mysql.h"
#include "server/database-drivers/pgsql/pgsql.h"
#include "server/job-workers/job-message.h"
#include "server/json-logger.h"
#include "server/numa-configuration.h"
#include "server/php-engine-vars.h"
#include "server/php-queries.h"
#include "server/php-query-data.h"
#include "server/php-runner.h"
#include "server/php-worker.h"
#include "server/server-config.h"
#include "server/shared-data-worker-cache.h"
#include "server/signal-handlers.h"
#include "server/workers-control.h"

static enum { QUERY_TYPE_NONE, QUERY_TYPE_CONSOLE, QUERY_TYPE_HTTP, QUERY_TYPE_RPC, QUERY_TYPE_JOB } query_type;
static bool is_head_query;

static const string HTTP_DATE("D, d M Y H:i:s \\G\\M\\T", 21);

static const int OB_MAX_BUFFERS = 50;
static int ob_cur_buffer;

static string_buffer oub[OB_MAX_BUFFERS];
string_buffer* coub;
constexpr int ob_system_level = 0;
static int http_need_gzip;

static bool is_utf8_enabled = false;
bool is_json_log_on_timeout_enabled = true;
bool is_demangled_stacktrace_logs_enabled = false;

static int ignore_level = 0;

mixed runtime_config;

mixed f$kphp_get_runtime_config() {
  return runtime_config;
}

void f$ob_clean() {
  coub->clean();
}

static inline void reset_gzip_header() {
  if (ob_cur_buffer == 0) {
    http_need_gzip &= ~4;
  }
}

bool f$ob_end_clean() {
  if (ob_cur_buffer == 0) {
    return false;
  }

  coub = &oub[--ob_cur_buffer];
  reset_gzip_header();
  return true;
}

Optional<string> f$ob_get_clean() {
  if (ob_cur_buffer == 0) {
    return false;
  }

  string result = coub->str();
  coub = &oub[--ob_cur_buffer];
  reset_gzip_header();
  return result;
}

string f$ob_get_contents() {
  return coub->str();
}

void f$ob_start(const string& callback) {
  if (ob_cur_buffer + 1 == OB_MAX_BUFFERS) {
    php_warning("Maximum nested level of output buffering reached. Can't do ob_start(%s)", callback.c_str());
    return;
  }
  if (!callback.empty()) {
    if (ob_cur_buffer == 0 && callback == string("ob_gzhandler")) {
      http_need_gzip |= 4;
    } else {
      php_critical_error("unsupported callback %s at buffering level %d", callback.c_str(), ob_cur_buffer + 1);
    }
  }

  coub = &oub[++ob_cur_buffer];
  coub->clean();
}

void f$ob_flush() {
  if (ob_cur_buffer == 0) {
    php_warning("ob_flush with no buffer opented");
    return;
  }
  --ob_cur_buffer;
  coub = &oub[ob_cur_buffer];
  print(oub[ob_cur_buffer + 1]);
  ++ob_cur_buffer;
  coub = &oub[ob_cur_buffer];
  f$ob_clean();
}

bool f$ob_end_flush() {
  if (ob_cur_buffer == 0) {
    return false;
  }
  f$ob_flush();
  return f$ob_end_clean();
}

Optional<string> f$ob_get_flush() {
  if (ob_cur_buffer == 0) {
    return false;
  }
  string result = coub->str();
  f$ob_flush();
  f$ob_end_clean();
  return result;
}

Optional<int64_t> f$ob_get_length() {
  if (ob_cur_buffer == 0) {
    return false;
  }
  return coub->size();
}

int64_t f$ob_get_level() {
  return ob_cur_buffer;
}

static int http_return_code;
static string http_status_line;
static char headers_storage[sizeof(array<string>)];
static array<string>* headers = reinterpret_cast<array<string>*>(headers_storage);
static long long header_last_query_num = -1;
static bool headers_custom_handler_invoked = false;
static bool headers_sent = false;
static headers_custom_handler_function_type headers_custom_handler_function;

static bool check_status_line_int(const char* str, int str_len, int* pos) {
  if (*pos != str_len && str[*pos] == '0') {
    (*pos)++;
    return true;
  }

  for (int i = 0; i <= 9; i++) { // allow up to 9 digits total
    if (*pos == str_len || str[*pos] < '0' || str[*pos] > '9') {
      return i > 0;
    }
    (*pos)++;
  }
  return false;
}

static bool check_status_line(const char* str, int str_len) {
  // skip check for beginning with "HTTP/"

  int pos = 5;
  if (!check_status_line_int(str, str_len, &pos)) {
    return false;
  }
  if (pos == str_len || str[pos] != '.') {
    return false;
  }
  pos++;
  if (!check_status_line_int(str, str_len, &pos)) {
    return false;
  }

  if (pos == str_len || str[pos] != ' ') {
    return false;
  }
  pos++;

  if (pos == str_len || str[pos] < '1' || str[pos] > '9') {
    return false;
  }
  pos++;
  if (pos == str_len || str[pos] < '0' || str[pos] > '9') {
    return false;
  }
  pos++;
  if (pos == str_len || str[pos] < '0' || str[pos] > '9') {
    return false;
  }
  pos++;

  if (pos == str_len || str[pos] != ' ') {
    return false;
  }
  pos++;

  while (pos != str_len) {
    if ((0 <= str[pos] && str[pos] <= 31) || str[pos] == 127) {
      return false;
    }
    pos++;
  }

  return true;
}

static void header(const char* str, int str_len, bool replace = true, int http_response_code = 0) {
  if (dl::query_num != header_last_query_num) {
    new (headers_storage) array<string>();
    header_last_query_num = dl::query_num;
  }

  // status line
  if (str_len >= 5 && !strncasecmp(str, "HTTP/", 5)) {
    if (check_status_line(str, str_len)) {
      http_status_line = string(str, str_len);
      int pos = 5;
      while (str[pos] != ' ') {
        pos++;
      }
      sscanf(str + pos, "%d", &http_return_code);
    } else {
      php_critical_error("wrong status line '%s' specified in function header", str);
    }
    return;
  }

  // regular header
  const char* p = strchr(str, ':');
  if (p == nullptr) {
    php_warning("Wrong header line specified: \"%s\"", str);
    return;
  }
  string name = f$trim(string(str, static_cast<string::size_type>(p - str)));
  if (strpbrk(name.c_str(), "()<>@,;:\\\"/[]?={}") != nullptr) {
    php_warning("Wrong header name: \"%s\"", name.c_str());
    return;
  }
  for (string::size_type i = 0; i < name.size(); i++) {
    if (name[i] <= 32 || name[i] >= 127) {
      php_warning("Wrong header name: \"%s\"", name.c_str());
      return;
    }
  }

  for (int i = (int)(p - str + 1); i < str_len; i++) {
    if ((0 <= str[i] && str[i] <= 31) || str[i] == 127) {
      php_warning("Wrong header value: \"%s\"", p + 1);
      return;
    }
  }

  string value = string(static_cast<string::size_type>(name.size() + (str_len - (p - str)) + 2), false);
  memcpy(value.buffer(), name.c_str(), name.size());
  memcpy(value.buffer() + name.size(), p, str_len - (p - str));
  value[value.size() - 2] = '\r';
  value[value.size() - 1] = '\n';

  name = f$strtolower(name);
  if (replace || !headers->has_key(name)) {
    headers->set_value(name, value);
  } else {
    (*headers)[name].append(value);
  }

  if (str_len >= 9 && !strncasecmp(str, "Location:", 9) && http_response_code == 0) {
    http_response_code = 302;
  }

  if (str_len && http_response_code > 0 && http_response_code != http_return_code) {
    http_return_code = http_response_code;
    http_status_line = string();
  }
}

void f$header(const string& str, bool replace, int64_t http_response_code) {
  header(str.c_str(), (int)str.size(), replace, static_cast<int32_t>(http_response_code));
}

array<string> f$headers_list() {
  array<string> result;
  if (dl::query_num != header_last_query_num) {
    new (headers_storage) array<string>();
    header_last_query_num = dl::query_num;
  }

  string delim("\r\n");

  for (auto header = headers->cbegin(); header != headers->cend(); ++header) {
    array<string> temp = f$explode(delim, header.get_value());
    for (auto part = temp.cbegin(); part != temp.cend(); ++part) {
      if (!part.get_value().empty()) {
        result.push_back(part.get_value());
      }
    }
  }

  return result;
}

Optional<string>& get_dummy_headers_sent_filename() noexcept {
  static Optional<string> filename;
  return filename;
}

Optional<int64_t>& get_dummy_headers_sent_line() noexcept {
  static Optional<int64_t> dummy_line;
  return dummy_line;
}

bool f$headers_sent([[maybe_unused]] Optional<string>& filename, [[maybe_unused]] Optional<int64_t>& line) {
  return headers_sent;
}

void f$send_http_103_early_hints(const array<string>& headers) {
  string header("HTTP/1.1 103 Early Hints\r\n");
  for (const auto& h : headers) {
    header.append(h.get_value().c_str()).append("\r\n");
  }
  http_send_immediate_response(header.c_str(), header.size(), "\r\n", 2);
}

void f$setrawcookie(const string& name, const string& value, int64_t expire, const string& path, const string& domain, bool secure, bool http_only) {
  string date = f$gmdate(HTTP_DATE, expire);

  kphp_runtime_context.static_SB_spare.clean() << "Set-Cookie: " << name << '=';
  if (value.empty()) {
    kphp_runtime_context.static_SB_spare << "DELETED; expires=Thu, 01 Jan 1970 00:00:01 GMT";
  } else {
    kphp_runtime_context.static_SB_spare << value;

    if (expire != 0) {
      kphp_runtime_context.static_SB_spare << "; expires=" << date;
    }
  }
  if (!path.empty()) {
    kphp_runtime_context.static_SB_spare << "; path=" << path;
  }
  if (!domain.empty()) {
    kphp_runtime_context.static_SB_spare << "; domain=" << domain;
  }
  if (secure) {
    kphp_runtime_context.static_SB_spare << "; secure";
  }
  if (http_only) {
    kphp_runtime_context.static_SB_spare << "; HttpOnly";
  }
  header(kphp_runtime_context.static_SB_spare.c_str(), (int)kphp_runtime_context.static_SB_spare.size(), false);
}

void f$setcookie(const string& name, const string& value, int64_t expire, const string& path, const string& domain, bool secure, bool http_only) {
  f$setrawcookie(name, f$urlencode(value), expire, path, domain, secure, http_only);
}

int64_t f$ignore_user_abort(Optional<bool> enable) {
  php_assert(php_worker.has_value() && php_worker->conn != nullptr);
  if (enable.is_null()) {
    return ignore_level;
  } else if (enable.val()) {
    php_worker->conn->ignored = true;
    return ignore_level++;
  } else {
    int prev = ignore_level > 0 ? ignore_level-- : 0;
    if (ignore_level == 0) {
      php_worker->conn->ignored = false;
    }
    if (php_worker->conn->interrupted && !php_worker->conn->ignored) {
      php_worker->conn->status = conn_error;
      f$exit(1);
    }
    return prev;
  }
}

static inline const char* http_get_error_msg_text(int* code) {
  if (*code == 200) {
    return "OK";
  }
  if (*code < 100 || *code > 999) {
    *code = 500;
  }
  switch (*code) {
  case 201:
    return "Created";
  case 202:
    return "Accepted";
  case 204:
    return "No Content";
  case 206:
    return "Partial Content";
  case 301:
    return "Moved Permanently";
  case 302:
    return "Found";
  case 303:
    return "See Other";
  case 304:
    return "Not Modified";
  case 307:
    return "Temporary Redirect";
  case 400:
    return "Bad Request";
  case 401:
    return "Unauthorized";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 406:
    return "Not Acceptable";
  case 408:
    return "Request Timeout";
  case 411:
    return "Length Required";
  case 413:
    return "Request Entity Too Large";
  case 414:
    return "Request-URI Too Long";
  case 418:
    return "I'm a teapot";
  case 480:
    return "Temporarily Unavailable";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  case 502:
    return "Bad Gateway";
  case 503:
    return "Service Unavailable";
  }
  return "Extension Code";
}

static void set_content_length_header(int content_length) {
  kphp_runtime_context.static_SB_spare.clean() << "Content-Length: " << content_length;
  header(kphp_runtime_context.static_SB_spare.c_str(), (int)kphp_runtime_context.static_SB_spare.size());
}

static const string_buffer* get_headers() { // can't use static_SB, returns pointer to kphp_runtime_context.static_SB_spare
  string date = f$gmdate(HTTP_DATE);
  kphp_runtime_context.static_SB_spare.clean() << "Date: " << date;
  header(kphp_runtime_context.static_SB_spare.c_str(), (int)kphp_runtime_context.static_SB_spare.size());

  php_assert(dl::query_num == header_last_query_num);

  kphp_runtime_context.static_SB_spare.clean();
  if (!http_status_line.empty()) {
    kphp_runtime_context.static_SB_spare << http_status_line << "\r\n";
  } else {
    const char* message = http_get_error_msg_text(&http_return_code);
    kphp_runtime_context.static_SB_spare << "HTTP/1.1 " << http_return_code << " " << message << "\r\n";
  }

  const array<string>* arr = headers;
  for (array<string>::const_iterator p = arr->begin(); p != arr->end(); ++p) {
    kphp_runtime_context.static_SB_spare << p.get_value();
  }
  kphp_runtime_context.static_SB_spare << "\r\n";

  return &kphp_runtime_context.static_SB_spare;
}

constexpr uint32_t MAX_SHUTDOWN_FUNCTIONS = 256;

namespace {

int shutdown_functions_count = 0;
char shutdown_function_storage[MAX_SHUTDOWN_FUNCTIONS * sizeof(shutdown_function_type)];
shutdown_function_type* const shutdown_functions = reinterpret_cast<shutdown_function_type*>(shutdown_function_storage);
shutdown_functions_status shutdown_functions_status_value = shutdown_functions_status::not_executed;
jmp_buf timeout_exit;
bool finished = false;

} // namespace

static const string_buffer* compress_http_query_body(string_buffer* http_query_body) {
  php_assert(http_query_body != nullptr);

  if (is_head_query) {
    http_query_body->clean();
    return http_query_body;
  } else {
    if ((http_need_gzip & 5) == 5) {
      header("Content-Encoding: gzip", 22, true);
      return zlib_encode(http_query_body->c_str(), http_query_body->size(), 6, ZLIB_ENCODING_GZIP);
    } else if ((http_need_gzip & 6) == 6) {
      header("Content-Encoding: deflate", 25, true);
      return zlib_encode(http_query_body->c_str(), http_query_body->size(), 6, ZLIB_ENCODING_DEFLATE);
    } else {
      return http_query_body;
    }
  }
}

static int ob_merge_buffers() {
  php_assert(ob_cur_buffer >= 0);
  int ob_first_not_empty = 0;
  while (ob_first_not_empty < ob_cur_buffer && oub[ob_first_not_empty].size() == 0) {
    ob_first_not_empty++;
  }
  for (int i = ob_first_not_empty + 1; i <= ob_cur_buffer; i++) {
    oub[ob_first_not_empty].append(oub[i].c_str(), oub[i].size());
  }
  return ob_first_not_empty;
}

void f$flush() {
  php_assert(ob_cur_buffer >= 0 && php_worker.has_value());
  // Run custom headers handler before body processing
  if (!headers_custom_handler_invoked && query_type == QUERY_TYPE_HTTP) {
    headers_custom_handler_invoked = true;
    if (headers_custom_handler_function) {
      headers_custom_handler_function();
    }
    headers_sent = true;
  }
  string_buffer const* http_body = compress_http_query_body(&oub[ob_system_level]);
  string_buffer const* http_headers = nullptr;
  if (!php_worker->flushed_http_connection) {
    http_headers = get_headers();
    php_worker->flushed_http_connection = true;
  }
  http_send_immediate_response(http_headers ? http_headers->buffer() : nullptr, http_headers ? http_headers->size() : 0, http_body->buffer(),
                               http_body->size());
  oub[ob_system_level].clean();
  kphp_runtime_context.static_SB_spare.clean();
}

void f$fastcgi_finish_request(int64_t exit_code) {
  // Run custom headers handler before body processing
  if (!headers_custom_handler_invoked && query_type == QUERY_TYPE_HTTP) {
    headers_custom_handler_invoked = true;
    if (headers_custom_handler_function) {
      headers_custom_handler_function();
    }
    headers_sent = true;
  }
  int ob_total_buffer = ob_merge_buffers();
  if (php_worker.has_value() && php_worker->flushed_http_connection) {
    string const raw_response = oub[ob_total_buffer].str();
    http_set_result(nullptr, 0, raw_response.c_str(), raw_response.size(), static_cast<int32_t>(exit_code));
    php_assert(0);
  }

  if (!run_once) {
    exit_code = 0; // TODO: is it correct?
  }

  switch (query_type) {
  case QUERY_TYPE_CONSOLE: {
    // TODO console_set_result
    fflush(stderr);

    write_safe(1, oub[ob_total_buffer].buffer(), oub[ob_total_buffer].size(), {});

    // TODO move to finish_script
    free_runtime_environment(PhpScriptMutableGlobals::current().get_superglobals());

    break;
  }
  case QUERY_TYPE_HTTP: {
    const string_buffer* compressed = compress_http_query_body(&oub[ob_total_buffer]);
    if (!is_head_query) {
      set_content_length_header(compressed->size());
    }
    const string_buffer* headers = get_headers();
    http_set_result(headers->buffer(), headers->size(), compressed->buffer(), compressed->size(), static_cast<int32_t>(exit_code));

    break;
  }
  case QUERY_TYPE_RPC: {
    rpc_set_result(oub[ob_total_buffer].buffer(), oub[ob_total_buffer].size(), static_cast<int32_t>(exit_code));

    break;
  }
  case QUERY_TYPE_JOB: {
    job_set_result(static_cast<int32_t>(exit_code));

    break;
  }
  default:
    php_assert(0);
    exit(1);
  }

  ob_cur_buffer = 0;
  coub = &oub[ob_cur_buffer];
  coub->clean();
}

void run_shutdown_functions(ShutdownType shutdown_type) {
  if (kphp_tracing::is_turned_on()) {
    kphp_tracing::on_shutdown_functions_start(static_cast<int>(shutdown_type));
  }

  php_assert(dl::is_malloc_replaced() == false);
  forcibly_stop_all_running_resumables();

  ShutdownProfiler shutdown_profiler;
  for (int i = 0; i < shutdown_functions_count; i++) {
    shutdown_functions[i]();
  }

  // don't wrap this call into if(kphp_tracing::is_turned_on()), intentionally
  kphp_tracing::on_php_script_finish_ok(f$get_net_time(), f$get_script_time());
}

shutdown_functions_status get_shutdown_functions_status() {
  return shutdown_functions_status_value;
}

int get_shutdown_functions_count() {
  return shutdown_functions_count;
}

void run_shutdown_functions_from_timeout() {
  shutdown_functions_status_value = shutdown_functions_status::running_from_timeout;
  // to safely run the shutdown handlers in the timeout context, we set
  // a recovery point to be used from the user-called die/exit;
  // without that, exit would lead to a finished state instead of the error state
  // we were about to enter (since timeout is an error state)
  reset_script_timeout();
  if (setjmp(timeout_exit) == 0) {
    run_shutdown_functions(ShutdownType::timeout);
  }
}

void run_shutdown_functions_from_script(ShutdownType shutdown_type) {
  shutdown_functions_status_value = shutdown_functions_status::running;
  // when running shutdown functions from a normal (non-timeout) context,
  // reset the timer to give shutdown functions a new span of time to avoid
  // prematurely terminated shutdown functions for long running scripts;
  // if shutdown functions can't finish with that time quota, they will
  // be interrupted as usual
  reset_script_timeout();
  run_shutdown_functions(shutdown_type);
}

void register_shutdown_function_impl(shutdown_function_type&& f) {
  if (shutdown_functions_count == MAX_SHUTDOWN_FUNCTIONS) {
    php_warning("Too many shutdown functions registered, ignore next one\n");
    return;
  }
  // this guard is to preserve correct state of 'shutdown_function_type' after construction
  // it's matter because the destructor of 'shutdown_function_type' is called now
  dl::CriticalSectionGuard critical_section;
  // I really need this, because this memory can contain random trash, if previouse script failed
  new (&shutdown_functions[shutdown_functions_count++]) shutdown_function_type{std::move(f)};
}

void register_header_handler_impl(headers_custom_handler_function_type&& f) {
  dl::CriticalSectionGuard critical_section;
  // Move assignment leads to lhs object invalidation and fires memory releasing mechanism
  // But memory is already released by destructor after previous run
  // Therefore we need to use placement new
  new (&headers_custom_handler_function) headers_custom_handler_function_type{std::move(f)};
}

void finish(int64_t exit_code, bool from_exit) {
  check_script_timeout();
  if (!finished) {
    finished = true;
    forcibly_stop_profiler();
    run_shutdown_functions_from_script(from_exit ? ShutdownType::exit : ShutdownType::normal);
  }

  f$fastcgi_finish_request(exit_code);

  finish_script(static_cast<int32_t>(exit_code));

  // unreachable
  php_assert(0);
}

void f$exit(const mixed& v) {
  if (shutdown_functions_status_value == shutdown_functions_status::running_from_timeout) {
    longjmp(timeout_exit, 1);
  }

  if (v.is_string()) {
    *coub << v;
    finish(0, true);
  } else {
    finish(v.to_int(), true);
  }
}

void f$die(const mixed& v) {
  f$exit(v);
}

Optional<array<string>> f$gethostbynamel(const string& name) {
  dl::enter_critical_section(); // OK
  struct hostent* hp = gethostbyname(name.c_str());
  if (hp == nullptr || hp->h_addr_list == nullptr) {
    dl::leave_critical_section();
    return false;
  }
  dl::leave_critical_section();

  array<string> result;
  for (int i = 0; hp->h_addr_list[i] != nullptr; i++) {
    dl::enter_critical_section(); // OK
    const char* ip = inet_ntoa(*(struct in_addr*)hp->h_addr_list[i]);
    dl::leave_critical_section();
    result.push_back(string(ip));
  }

  return result;
}

Optional<string> f$inet_pton(const string& address) {
  int af, size;
  if (strchr(address.c_str(), ':')) {
    af = AF_INET6;
    size = 16;
  } else if (strchr(address.c_str(), '.')) {
    af = AF_INET;
    size = 4;
  } else {
    php_warning("Unrecognized address \"%s\"", address.c_str());
    return false;
  }

  char buffer[17] = {0};
  dl::enter_critical_section(); // OK
  if (inet_pton(af, address.c_str(), buffer) <= 0) {
    dl::leave_critical_section();
    php_warning("Unrecognized address \"%s\"", address.c_str());
    return false;
  }
  dl::leave_critical_section();

  return string(buffer, size);
}

extern bool run_once;

void print(const char* s, size_t s_len) {
  if (run_once && ob_cur_buffer == 0) {
    dl::CriticalSectionGuard critical_section;
    write(kstdout, s, s_len);
  } else {
    coub->append(s, s_len);
  }
}

void print(const char* s) {
  print(s, strlen(s));
}

void print(const string& s) {
  print(s.c_str(), s.size());
}

void print(const string_buffer& sb) {
  print(sb.buffer(), sb.size());
}

void dbg_echo(const char* s, size_t s_len) {
  dl::CriticalSectionGuard critical_section;
  write(kstderr, s, s_len);
}

void dbg_echo(const char* s) {
  dbg_echo(s, strlen(s));
}

void dbg_echo(const string& s) {
  dbg_echo(s.c_str(), s.size());
}

void dbg_echo(const string_buffer& sb) {
  dbg_echo(sb.buffer(), sb.size());
}

bool f$get_magic_quotes_gpc() {
  return false;
}

static string php_sapi_name() {
  switch (query_type) {
  case QUERY_TYPE_CONSOLE:
    return string("cli");
  case QUERY_TYPE_HTTP:
    return string("Kitten PHP");
  case QUERY_TYPE_RPC:
    if (run_once) {
      return string("cli");
    } else {
      return string("Kitten PHP");
    }
  case QUERY_TYPE_JOB:
    return string("KPHP job");
  default:
    php_assert(0);
    exit(1);
  }
}

string f$php_sapi_name() {
  return PhpScriptMutableGlobals::current().get_superglobals().v$d$PHP_SAPI;
}

static std::aligned_storage_t<sizeof(array<bool>), alignof(array<bool>)> uploaded_files_storage;
static array<bool>* uploaded_files = reinterpret_cast<array<bool>*>(&uploaded_files_storage);
static long long uploaded_files_last_query_num = -1;

static const int MAX_FILES = 100;

static string raw_post_data;

bool f$is_uploaded_file(const string& filename) {
  return (dl::query_num == uploaded_files_last_query_num && uploaded_files->get_value(filename) == 1);
}

bool f$move_uploaded_file(const string& oldname, const string& newname) {
  if (!f$is_uploaded_file(oldname)) {
    return false;
  }

  dl::enter_critical_section(); // NOT OK: uploaded_files
  if (f$rename(oldname, newname)) {
    uploaded_files->unset(oldname);
    dl::leave_critical_section();
    return true;
  }
  dl::leave_critical_section();

  return false;
}

class post_reader {
  char* buf;
  int post_len;
  int buf_pos;
  int buf_len;

  const string boundary;

  post_reader(const post_reader&);           // DISABLE copy constructor
  post_reader operator=(const post_reader&); // DISABLE copy assignment

public:
  post_reader(const char* post, int post_len, const string& boundary)
      : post_len(post_len),
        buf_pos(0),
        boundary(boundary) {
    if (post == nullptr) {
      buf = StringLibContext::get().static_buf.get();
      buf_len = 0;
    } else {
      buf = (char*)post;
      buf_len = post_len;
    }
  }

  int operator[](int i) {
    php_assert(i >= buf_pos);
    php_assert(i <= post_len);
    if (i >= post_len) {
      return 0;
    }

    i -= buf_pos;
    while (i >= buf_len) {
      int left = post_len - buf_pos - buf_len;
      int chunk_size = (int)boundary.size() + 65536 + 10;
      //      fprintf (stderr, "Load at pos %d. i = %d, buf_len = %d, left = %d, chunk_size = %d\n", i + buf_pos, i, buf_len, left, chunk_size);
      if (buf_len > 0) {
        int to_leave = chunk_size;
        int to_erase = buf_len - to_leave;

        php_assert(left > 0);
        php_assert(to_erase >= to_leave);

        memcpy(buf, buf + to_erase, to_leave);
        buf_pos += to_erase;
        i -= to_erase;

        buf_len = to_leave + http_load_long_query(buf + to_leave, min(to_leave, left), min(StringLibContext::STATIC_BUFFER_LENGTH - to_leave, left));
      } else {
        buf_len = http_load_long_query(buf, min(2 * chunk_size, left), min(StringLibContext::STATIC_BUFFER_LENGTH, left));
      }
    }

    return buf[i];
  }

  bool is_boundary(int i) {
    php_assert(i >= buf_pos);
    php_assert(i <= post_len);
    if (i >= post_len) {
      return true;
    }

    if (i > 0) {
      if ((*this)[i] == '\r') {
        i++;
      }
      if ((*this)[i] == '\n') {
        i++;
      } else {
        return false;
      }
    }
    if ((*this)[i] == '-' && (*this)[i + 1] == '-') {
      i += 2;
    } else {
      return false;
    }

    if (i + (int)boundary.size() > post_len) {
      return false;
    }

    if (i - buf_pos + (int)boundary.size() <= buf_len) {
      return !memcmp(buf + i - buf_pos, boundary.c_str(), boundary.size());
    }

    for (int j = 0; j < (int)boundary.size(); j++) {
      if ((*this)[i + j] != boundary[j]) {
        return false;
      }
    }
    return true;
  }

  int upload_file(const string& file_name, int& pos, int64_t max_file_size) {
    php_assert(pos > 0 && buf_len > 0 && buf_pos <= pos && pos <= post_len);

    if (pos == post_len) {
      return -UPLOAD_ERR_PARTIAL;
    }

    if (!f$is_writeable(file_name)) {
      return -UPLOAD_ERR_CANT_WRITE;
    }

    dl::enter_critical_section(); // OK
    int file_fd = open_safe(file_name.c_str(), O_WRONLY | O_TRUNC, 0644);
    if (file_fd < 0) {
      dl::leave_critical_section();
      return -UPLOAD_ERR_NO_FILE;
    }

    struct stat stat_buf;
    if (fstat(file_fd, &stat_buf) < 0) {
      close_safe(file_fd);
      dl::leave_critical_section();
      return -UPLOAD_ERR_CANT_WRITE;
    }
    dl::leave_critical_section();

    php_assert(S_ISREG(stat_buf.st_mode));

    if (buf_len == post_len) {
      int i = pos;
      while (!is_boundary(i)) {
        i++;
      }
      if (i == post_len) {
        dl::enter_critical_section(); // OK
        close_safe(file_fd);
        dl::leave_critical_section();
        return -UPLOAD_ERR_PARTIAL;
      }

      int file_size = i - pos;
      if (file_size > max_file_size) {
        dl::enter_critical_section(); // OK
        close_safe(file_fd);
        dl::leave_critical_section();
        return -UPLOAD_ERR_FORM_SIZE;
      }

      dl::enter_critical_section(); // OK
      if (write_safe(file_fd, buf + pos, (size_t)file_size, file_name) < (ssize_t)file_size) {
        file_size = -UPLOAD_ERR_CANT_WRITE;
      }

      close_safe(file_fd);
      dl::leave_critical_section();
      return file_size;
    } else {
      long long file_size = 0;
      const char* s;

      while (true) {
        (*this)[pos];
        int64_t i = pos - buf_pos;

        while (true) {
          php_assert(0 <= i && i <= buf_len);

          s = static_cast<const char*>(memmem(buf + i, buf_len - i, boundary.c_str(), boundary.size()));
          if (s == nullptr) {
            break;
          }

          int64_t r = s - buf;
          if (r > i + 2 && buf[r - 1] == '-' && buf[r - 2] == '-' && buf[r - 3] == '\n') {
            r -= 3;
            if (r > i && buf[r - 1] == '\r') {
              r--;
            }

            s = buf + r;
            break;
          }

          i = r + 1;
        }
        if (s != nullptr) {
          break;
        }

        int left = post_len - buf_pos - buf_len;
        int to_leave = (int)boundary.size() + 10;
        int to_erase = buf_len - to_leave;
        int to_write = to_erase - (pos - buf_pos);

        //        fprintf (stderr, "Load at pos %d. buf_len = %d, left = %d, to_leave = %d, to_erase = %d, to_write = %d.\n", buf_len + buf_pos, buf_len, left,
        //        to_leave, to_erase, to_write);

        if (left == 0) {
          dl::enter_critical_section(); // OK
          close_safe(file_fd);
          dl::leave_critical_section();
          return -UPLOAD_ERR_PARTIAL;
        }
        file_size += to_write;
        if (file_size > max_file_size) {
          dl::enter_critical_section(); // OK
          close_safe(file_fd);
          dl::leave_critical_section();
          return -UPLOAD_ERR_FORM_SIZE;
        }

        php_assert(to_erase >= to_leave);

        dl::enter_critical_section(); // OK
        if (write_safe(file_fd, buf + pos - buf_pos, (size_t)to_write, file_name) < (ssize_t)to_write) {
          close_safe(file_fd);
          dl::leave_critical_section();
          return -UPLOAD_ERR_CANT_WRITE;
        }
        dl::leave_critical_section();

        memcpy(buf, buf + to_erase, to_leave);
        buf_pos += to_erase;
        pos += to_write;

        buf_len = to_leave + http_load_long_query(buf + to_leave, min(StringLibContext::STATIC_BUFFER_LENGTH - to_leave, left),
                                                  min(StringLibContext::STATIC_BUFFER_LENGTH - to_leave, left));
      }

      php_assert(s != nullptr);

      dl::enter_critical_section(); // OK
      int to_write = (int)(s - (buf + pos - buf_pos));
      if (write_safe(file_fd, buf + pos - buf_pos, (size_t)to_write, file_name) < (ssize_t)to_write) {
        close_safe(file_fd);
        dl::leave_critical_section();
        return -UPLOAD_ERR_CANT_WRITE;
      }
      pos += to_write;

      close_safe(file_fd);
      dl::leave_critical_section();

      return (int)(file_size + to_write);
    }
  }
};

static int parse_multipart_one(post_reader& data, int i, mixed& v$_POST, mixed& v$_FILES) {
  string content_type("text/plain", 10);
  string name;
  string filename;
  int64_t max_file_size = std::numeric_limits<int64_t>::max();
  while (!data.is_boundary(i) && 33 <= data[i] && data[i] <= 126) {
    int j;
    string header_name;
    for (j = i; !data.is_boundary(j) && 33 <= data[j] && data[j] <= 126 && data[j] != ':'; j++) {
      header_name.push_back(static_cast<char>(data[j]));
    }
    if (data[j] != ':') {
      return j;
    }

    header_name = f$strtolower(header_name);
    i = j + 1;

    string header_value;
    do {
      while (!data.is_boundary(i + 1) && (data[i] != '\r' || data[i + 1] != '\n') && data[i] != '\n') {
        header_value.push_back(static_cast<char>(data[i++]));
      }
      if (data[i] == '\r') {
        i++;
      }
      if (data[i] == '\n') {
        i++;
      }

      if (data.is_boundary(i) || (33 <= data[i] && data[i] <= 126) || (data[i] == '\r' && data[i + 1] == '\n') || data[i] == '\n') {
        break;
      }
    } while (true);
    header_value = f$trim(header_value);

    if (!strcmp(header_name.c_str(), "content-disposition")) {
      if (strncmp(header_value.c_str(), "form-data;", 10)) {
        return i;
      }
      const char* p = header_value.c_str() + 10;
      while (true) {
        while (*p && *p == ' ') {
          p++;
        }
        if (*p == 0) {
          break;
        }
        const char* p_end = p;
        while (*p_end && *p_end != '=') {
          p_end++;
        }
        if (*p_end == 0) {
          break;
        }
        const string key = f$trim(string(p, static_cast<string::size_type>(p_end - p)));
        p = ++p_end;
        while (*p_end && *p_end != ';') {
          p_end++;
        }
        string value = f$trim(string(p, static_cast<string::size_type>(p_end - p)));
        if ((int)value.size() > 1 && value[0] == '"' && value[value.size() - 1] == '"') {
          value.assign(value, 1, value.size() - 2);
        }
        p = p_end;
        if (*p) {
          p++;
        }

        if (!strcmp(key.c_str(), "name")) {
          name = value;
        } else if (!strcmp(key.c_str(), "filename")) {
          filename = value;
        } else {
          //          fprintf (stderr, "Unknown key %s\n", key.c_str());
        }
      }
    } else if (!strcmp(header_name.c_str(), "content-type")) {
      content_type = f$strtolower(header_value);
    } else if (!strcmp(header_name.c_str(), "max-file-size")) {
      max_file_size = header_value.to_int();
    } else {
      //      fprintf (stderr, "Unknown header %s\n", header_name.c_str());
    }
  }
  if (data.is_boundary(i)) {
    return i;
  }
  if (data[i] == '\r') {
    i++;
  }
  if (data[i] == '\n') {
    i++;
  } else {
    return i;
  }

  if (filename.empty()) {
    if (!name.empty()) {
      string post_data;
      while (!data.is_boundary(i) && (int)post_data.size() < 65536) {
        post_data.push_back(static_cast<char>(data[i++]));
      }
      if ((int)post_data.size() < 65536) {
        if (!strncmp(content_type.c_str(), "application/x-www-form-urlencoded", 33)) {
          f$parse_str(post_data, v$_POST[name]);
        } else {
          // TODO
          v$_POST.set_value(name, post_data);
        }
      }
    }
  } else {
    int file_size;
    Optional<string> tmp_name;
    if (v$_FILES.count() < MAX_FILES) {
      if (dl::query_num != uploaded_files_last_query_num) {
        new (&uploaded_files_storage) array<bool>();
        uploaded_files_last_query_num = dl::query_num;
      }

      dl::enter_critical_section(); // NOT OK: uploaded_files
      tmp_name = f$tempnam(string(), string());
      uploaded_files->set_value(tmp_name.val(), true);
      dl::leave_critical_section();

      if (f$boolval(tmp_name)) {
        file_size = data.upload_file(tmp_name.val(), i, max_file_size);

        if (file_size < 0) {
          dl::enter_critical_section(); // NOT OK: uploaded_files
          f$unlink(tmp_name.val());
          uploaded_files->unset(tmp_name.val());
          dl::leave_critical_section();
        }
      } else {
        file_size = -UPLOAD_ERR_NO_TMP_DIR;
      }
    } else {
      file_size = -UPLOAD_ERR_NO_FILE;
    }

    if (name.size() >= 2 && name[name.size() - 2] == '[' && name[name.size() - 1] == ']') {
      mixed& file = v$_FILES[name.substr(0, name.size() - 2)];
      if (file_size >= 0) {
        file[string("name")].push_back(filename);
        file[string("type")].push_back(content_type);
        file[string("size")].push_back(file_size);
        file[string("tmp_name")].push_back(tmp_name.val());
        file[string("error")].push_back(UPLOAD_ERR_OK);
      } else {
        file[string("name")].push_back(string());
        file[string("type")].push_back(string());
        file[string("size")].push_back(0);
        file[string("tmp_name")].push_back(string());
        file[string("error")].push_back(-file_size);
      }
    } else {
      mixed& file = v$_FILES[name];
      if (file_size >= 0) {
        file.set_value(string("name"), filename);
        file.set_value(string("type"), content_type);
        file.set_value(string("size"), file_size);
        file.set_value(string("tmp_name"), tmp_name.val());
        file.set_value(string("error"), UPLOAD_ERR_OK);
      } else {
        file.set_value(string("size"), 0);
        file.set_value(string("tmp_name"), string());
        file.set_value(string("error"), -file_size);
      }
    }
  }

  return i;
}

static bool parse_multipart(const char* post, int post_len, const string& boundary, mixed& v$_POST, mixed& v$_FILES) {
  static const int MAX_BOUNDARY_LENGTH = 70;

  if (boundary.empty() || (int)boundary.size() > MAX_BOUNDARY_LENGTH) {
    return false;
  }
  php_assert(post_len >= 0);

  post_reader data(post, post_len, boundary);

  for (int i = 0; i < post_len; i++) {
    //    fprintf (stderr, "!!!! %d\n", i);
    i = parse_multipart_one(data, i, v$_POST, v$_FILES);

    //    fprintf (stderr, "???? %d\n", i);
    while (!data.is_boundary(i)) {
      i++;
    }
    i += 2 + (int)boundary.size();

    while (i < post_len && data[i] != '\n') {
      i++;
    }
  }

  return true;
}

static char arg_vars_storage[sizeof(array<string>)];
static array<string>* arg_vars = nullptr;

Optional<int64_t>& get_dummy_rest_index() noexcept {
  static Optional<int64_t> dummy_rest_index;
  return dummy_rest_index;
}

Optional<array<mixed>> f$getopt(const string& options, array<string> longopts, Optional<int64_t>& rest_index) {
  if (!arg_vars) {
    return false;
  }
  string real_options{"+"};
  real_options.append(options);
  const char* php_argv[arg_vars->count()];
  int php_argc = 0;
  for (auto iter : *arg_vars) {
    php_argv[php_argc++] = iter.get_value().c_str();
  }

  option real_longopts[longopts.count() + 1];
  int longopts_count = 0;
  for (auto iter : longopts) {
    string opt = iter.get_value();
    string::size_type count = 0;
    while (count < opt.size() && opt[opt.size() - count - 1] == ':') {
      count++;
    }
    if (count > 2 || count == opt.size()) {
      return false;
    }
    iter.get_value() = opt.substr(0, opt.size() - count);
    real_longopts[longopts_count].name = iter.get_value().c_str();
    real_longopts[longopts_count].flag = nullptr;
    real_longopts[longopts_count].val = 300 + longopts_count;
    real_longopts[longopts_count].has_arg = (count == 0 ? no_argument : (count == 1 ? required_argument : optional_argument));
    longopts_count++;
  }
  real_longopts[longopts_count] = option();

  array<mixed> result;

  optind = 0;
  while (int i = getopt_long(php_argc, (char* const*)php_argv, real_options.c_str(), real_longopts, nullptr)) {
    if (i == -1 || i == '?') {
      break;
    }
    string key = (i < 255 ? string(1, (char)i) : string(real_longopts[i - 300].name));
    mixed value;
    if (optarg) {
      value = string(optarg);
    } else {
      value = false;
    }

    if (result.has_key(key)) {
      if (!f$is_array(result.get_value(key))) {
        result.set_value(key, array<mixed>::create(result.get_value(key), value));
      } else {
        result[key].push_back(value);
      }
    } else {
      result.set_value(key, value);
    }
  }
  rest_index = optind;

  return result;
}

void arg_add(const char* value) {
  php_assert(dl::query_num == 0);

  if (arg_vars == nullptr) {
    new (arg_vars_storage) array<string>();
    arg_vars = reinterpret_cast<array<string>*>(arg_vars_storage);
  }

  arg_vars->push_back(string(value));
}

static void reset_superglobals(PhpScriptBuiltInSuperGlobals& superglobals) {
  dl::enter_critical_section();

  hard_reset_var(superglobals.v$_SERVER, array<mixed>());
  hard_reset_var(superglobals.v$_GET, array<mixed>());
  hard_reset_var(superglobals.v$_POST, array<mixed>());
  hard_reset_var(superglobals.v$_ENV, array<mixed>());
  hard_reset_var(superglobals.v$_FILES, array<mixed>());
  hard_reset_var(superglobals.v$_COOKIE, array<mixed>());
  hard_reset_var(superglobals.v$_REQUEST, array<mixed>());

  dl::leave_critical_section();
}

// RFC link: https://tools.ietf.org/html/rfc2617#section-2
// Header example:
//  Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==
static void parse_http_authorization_header(const string& header_value, mixed& v$_SERVER) {
  array<string> header_parts = explode(' ', header_value);
  if (header_parts.count() != 2) {
    return;
  }
  const string& auth_scheme = header_parts[0];
  const string& auth_credentials = header_parts[1];
  if (auth_scheme != string("Basic")) {
    return;
  }
  auto decoded_login_pass = f$base64_decode(auth_credentials, true);
  if (!decoded_login_pass.has_value()) {
    return;
  }
  array<string> auth_data = explode(':', decoded_login_pass.val());
  if (auth_data.count() != 2) {
    return;
  }
  v$_SERVER.set_value(string("PHP_AUTH_USER"), auth_data[0]);
  v$_SERVER.set_value(string("PHP_AUTH_PW"), auth_data[1]);
  v$_SERVER.set_value(string("AUTH_TYPE"), auth_scheme);
}

static void save_rpc_query_headers(const tl_query_header_t& header, mixed& v$_SERVER) {
  namespace flag = vk::tl::common::rpc_invoke_req_extra_flags;

  if (header.actor_id) {
    v$_SERVER.set_value(string("RPC_ACTOR_ID"), static_cast<int64_t>(header.actor_id));
  }
  if (header.flags) {
    v$_SERVER.set_value(string("RPC_EXTRA_FLAGS"), header.flags);
  }
  if (header.flags & flag::wait_binlog_pos) {
    v$_SERVER.set_value(string("RPC_EXTRA_WAIT_BINLOG_POS"), static_cast<int64_t>(header.wait_binlog_pos));
  }
  if (header.flags & flag::string_forward_keys) {
    array<string> string_forward_keys;
    string_forward_keys.reserve(header.string_forward_keys.size(), true);
    for (const auto& str_key : header.string_forward_keys) {
      string_forward_keys.emplace_back(string(str_key.data(), str_key.size()));
    }
    v$_SERVER.set_value(string("RPC_EXTRA_STRING_FORWARD_KEYS"), std::move(string_forward_keys));
  }
  if (header.flags & flag::int_forward_keys) {
    array<int64_t> int_forward_keys;
    int_forward_keys.reserve(header.int_forward_keys.size(), true);
    for (int int_key : header.int_forward_keys) {
      int_forward_keys.emplace_back(int_key);
    }
    v$_SERVER.set_value(string("RPC_EXTRA_INT_FORWARD_KEYS"), std::move(int_forward_keys));
  }
  if (header.flags & flag::string_forward) {
    v$_SERVER.set_value(string("RPC_EXTRA_STRING_FORWARD"), string(header.string_forward.data(), header.string_forward.size()));
  }
  if (header.flags & flag::int_forward) {
    v$_SERVER.set_value(string("RPC_EXTRA_INT_FORWARD"), static_cast<int64_t>(header.int_forward));
  }
  if (header.flags & flag::custom_timeout_ms) {
    v$_SERVER.set_value(string("RPC_EXTRA_CUSTOM_TIMEOUT_MS"), header.custom_timeout);
  }
  if (header.flags & flag::supported_compression_version) {
    v$_SERVER.set_value(string("RPC_EXTRA_SUPPORTED_COMPRESSION_VERSION"), header.supported_compression_version);
  }
  if (header.flags & flag::random_delay) {
    v$_SERVER.set_value(string("RPC_EXTRA_RANDOM_DELAY"), header.random_delay);
  }
}

static void init_superglobals_impl(const http_query_data& http_data, const rpc_query_data& rpc_data, const job_query_data& job_data,
                                   PhpScriptBuiltInSuperGlobals& superglobals) {
  rpc_parse(rpc_data.data.data(), rpc_data.data.size());

  reset_superglobals(superglobals);

  if (query_type == QUERY_TYPE_JOB) {
    superglobals.v$_SERVER.set_value(string("JOB_ID"), job_data.job_request->job_id);
    init_job_server_interface_lib(job_data);
  }

  string uri_str;
  if (http_data.uri_len) {
    uri_str.assign(http_data.uri, http_data.uri_len);
    superglobals.v$_SERVER.set_value(string("PHP_SELF"), uri_str);
    superglobals.v$_SERVER.set_value(string("SCRIPT_URL"), uri_str);
    superglobals.v$_SERVER.set_value(string("SCRIPT_NAME"), uri_str);
  }

  string get_str;
  if (http_data.get_len) {
    get_str.assign(http_data.get, http_data.get_len);
    f$parse_str(get_str, superglobals.v$_GET);

    superglobals.v$_SERVER.set_value(string("QUERY_STRING"), get_str);
  }

  if (http_data.uri) {
    if (http_data.get_len) {
      superglobals.v$_SERVER.set_value(string("REQUEST_URI"), (kphp_runtime_context.static_SB.clean() << uri_str << '?' << get_str).str());
    } else {
      superglobals.v$_SERVER.set_value(string("REQUEST_URI"), uri_str);
    }
  }

  http_need_gzip = 0;
  string content_type("application/x-www-form-urlencoded", 33);
  string content_type_lower = content_type;
  if (http_data.headers_len) {
    int i = 0;
    while (i < http_data.headers_len && 33 <= http_data.headers[i] && http_data.headers[i] <= 126) {
      int j;
      for (j = i; j < http_data.headers_len && 33 <= http_data.headers[j] && http_data.headers[j] <= 126 && http_data.headers[j] != ':'; j++) {
      }
      if (http_data.headers[j] != ':') {
        break;
      }

      string header_name = f$strtolower(string(http_data.headers + i, j - i));
      i = j + 1;

      string header_value;
      do {
        while (i < http_data.headers_len && http_data.headers[i] != '\r' && http_data.headers[i] != '\n') {
          header_value.push_back(http_data.headers[i++]);
        }

        while (i < http_data.headers_len && (http_data.headers[i] == '\r' || http_data.headers[i] == '\n')) {
          i++;
        }

        if (i == http_data.headers_len || (33 <= http_data.headers[i] && http_data.headers[i] <= 126)) {
          break;
        }
      } while (true);
      header_value = f$trim(header_value);

      if (!strcmp(header_name.c_str(), "accept-encoding")) {
        if (strstr(header_value.c_str(), "gzip") != nullptr) {
          http_need_gzip |= 1;
        }
        if (strstr(header_value.c_str(), "deflate") != nullptr) {
          http_need_gzip |= 2;
        }
      } else if (!strcmp(header_name.c_str(), "cookie")) {
        array<string> cookie = explode(';', header_value);
        for (int t = 0; t < (int)cookie.count(); t++) {
          array<string> cur_cookie = explode('=', f$trim(cookie[t]), 2);
          if ((int)cur_cookie.count() == 2) {
            parse_str_set_value(superglobals.v$_COOKIE, cur_cookie[0], f$urldecode(cur_cookie[1]));
          }
        }
      } else if (!strcmp(header_name.c_str(), "host")) {
        superglobals.v$_SERVER.set_value(string("SERVER_NAME"), header_value);
      } else if (!strcmp(header_name.c_str(), "authorization")) {
        parse_http_authorization_header(header_value, superglobals.v$_SERVER);
      }

      if (!strcmp(header_name.c_str(), "content-type")) {
        content_type = header_value;
        content_type_lower = f$strtolower(header_value);
      } else if (!strcmp(header_name.c_str(), "content-length")) {
        // must be equal to http_data.post_len, ignored
      } else {
        string key(header_name.size() + 5, false);
        bool good_name = true;
        for (int i = 0; i < (int)header_name.size(); i++) {
          if ('a' <= header_name[i] && header_name[i] <= 'z') {
            key[i + 5] = (char)(header_name[i] + 'A' - 'a');
          } else if ('0' <= header_name[i] && header_name[i] <= '9') {
            key[i + 5] = header_name[i];
          } else if ('-' == header_name[i]) {
            key[i + 5] = '_';
          } else {
            good_name = false;
            break;
          }
        }
        if (good_name) {
          key[0] = 'H';
          key[1] = 'T';
          key[2] = 'T';
          key[3] = 'P';
          key[4] = '_';
          superglobals.v$_SERVER.set_value(key, header_value);
        } else {
          //          fprintf (stderr, "%s : %s\n", header_name.c_str(), header_value.c_str());
        }
      }
    }
  }

  string HTTP_X_REAL_SCHEME("HTTP_X_REAL_SCHEME", 18);
  string HTTP_X_REAL_HOST("HTTP_X_REAL_HOST", 16);
  string HTTP_X_REAL_REQUEST("HTTP_X_REAL_REQUEST", 19);
  if (superglobals.v$_SERVER.isset(HTTP_X_REAL_SCHEME) && superglobals.v$_SERVER.isset(HTTP_X_REAL_HOST) && superglobals.v$_SERVER.isset(HTTP_X_REAL_REQUEST)) {
    string script_uri(superglobals.v$_SERVER.get_value(HTTP_X_REAL_SCHEME).to_string());
    script_uri.append("://", 3);
    script_uri.append(superglobals.v$_SERVER.get_value(HTTP_X_REAL_HOST).to_string());
    script_uri.append(superglobals.v$_SERVER.get_value(HTTP_X_REAL_REQUEST).to_string());
    superglobals.v$_SERVER.set_value(string("SCRIPT_URI"), script_uri);
  }

  if (http_data.post_len > 0) {
    bool is_parsed = (http_data.post != nullptr);
    //    fprintf (stderr, "!!!%.*s!!!\n", http_data.post_len, http_data.post);
    if (strstr(content_type_lower.c_str(), "application/x-www-form-urlencoded")) {
      if (http_data.post != nullptr) {
        dl::enter_critical_section(); // OK
        raw_post_data.assign(http_data.post, http_data.post_len);
        dl::leave_critical_section();

        f$parse_str(raw_post_data, superglobals.v$_POST);
      }
    } else if (strstr(content_type_lower.c_str(), "multipart/form-data")) {
      const char* p = strstr(content_type_lower.c_str(), "boundary");
      if (p) {
        p += 8;
        p = strchr(content_type.c_str() + (p - content_type_lower.c_str()), '=');
        if (p) {
          //          fprintf (stderr, "!!%s!!\n", p);
          p++;
          const char* end_p = strchrnul(p, ';');
          if (*p == '"' && p + 1 < end_p && end_p[-1] == '"') {
            p++;
            end_p--;
          }
          //          fprintf (stderr, "!%s!\n", p);
          is_parsed |= parse_multipart(http_data.post, http_data.post_len, string(p, static_cast<string::size_type>(end_p - p)), superglobals.v$_POST,
                                       superglobals.v$_FILES);
        }
      }
    } else {
      if (http_data.post != nullptr) {
        dl::enter_critical_section(); // OK
        raw_post_data.assign(http_data.post, http_data.post_len);
        dl::leave_critical_section();
      }
    }

    if (!is_parsed) {
      int loaded = 0;
      while (loaded < http_data.post_len) {
        int to_load = min(StringLibContext::STATIC_BUFFER_LENGTH, http_data.post_len - loaded);
        http_load_long_query(StringLibContext::get().static_buf.get(), to_load, to_load);
        loaded += to_load;
      }
    }

    superglobals.v$_SERVER.set_value(string("CONTENT_TYPE"), content_type);
  }

  double cur_time = microtime();
  superglobals.v$_SERVER.set_value(string("GATEWAY_INTERFACE"), string("CGI/1.1"));
  if (http_data.ip) {
    superglobals.v$_SERVER.set_value(string("REMOTE_ADDR"), f$long2ip(static_cast<int>(http_data.ip)));
  }
  if (http_data.port) {
    superglobals.v$_SERVER.set_value(string("REMOTE_PORT"), static_cast<int>(http_data.port));
  }
  if (rpc_data.header.qid) {
    superglobals.v$_SERVER.set_value(string("RPC_REQUEST_ID"), f$strval(static_cast<int64_t>(rpc_data.header.qid)));
    save_rpc_query_headers(rpc_data.header, superglobals.v$_SERVER);
    superglobals.v$_SERVER.set_value(string("RPC_REMOTE_IP"), static_cast<int>(rpc_data.remote_pid.ip));
    superglobals.v$_SERVER.set_value(string("RPC_REMOTE_PORT"), static_cast<int>(rpc_data.remote_pid.port));
    superglobals.v$_SERVER.set_value(string("RPC_REMOTE_PID"), static_cast<int>(rpc_data.remote_pid.pid));
    superglobals.v$_SERVER.set_value(string("RPC_REMOTE_UTIME"), rpc_data.remote_pid.utime);
  }
  is_head_query = false;
  if (http_data.request_method_len) {
    superglobals.v$_SERVER.set_value(string("REQUEST_METHOD"), string(http_data.request_method, http_data.request_method_len));
    if (http_data.request_method_len == 4 && !strncmp(http_data.request_method, "HEAD", http_data.request_method_len)) {
      is_head_query = true;
    }
  }
  superglobals.v$_SERVER.set_value(string("REQUEST_TIME"), int(cur_time));
  superglobals.v$_SERVER.set_value(string("REQUEST_TIME_FLOAT"), cur_time);
  superglobals.v$_SERVER.set_value(string("SERVER_PORT"), string("80"));
  superglobals.v$_SERVER.set_value(string("SERVER_PROTOCOL"), string("HTTP/1.1"));
  superglobals.v$_SERVER.set_value(string("SERVER_SIGNATURE"),
                                   (kphp_runtime_context.static_SB.clean() << "Apache/2.2.9 (Debian) PHP/5.2.6-1<<lenny10 with Suhosin-Patch Server at "
                                                                           << superglobals.v$_SERVER[string("SERVER_NAME")] << " Port 80")
                                       .str());
  superglobals.v$_SERVER.set_value(string("SERVER_SOFTWARE"), string("Apache/2.2.9 (Debian) PHP/5.2.6-1+lenny10 with Suhosin-Patch"));

  if (environ != nullptr) {
    for (int i = 0; environ[i] != nullptr; i++) {
      const char* s = strchr(environ[i], '=');
      php_assert(s != nullptr);
      superglobals.v$_ENV.set_value(string(environ[i], static_cast<string::size_type>(s - environ[i])), string(s + 1));
    }
  }

  superglobals.v$_REQUEST.as_array("") += superglobals.v$_GET.to_array();
  superglobals.v$_REQUEST.as_array("") += superglobals.v$_POST.to_array();
  superglobals.v$_REQUEST.as_array("") += superglobals.v$_COOKIE.to_array();

  if (http_data.uri != nullptr) {
    if (http_data.keep_alive) {
      header("Connection: keep-alive", 22);
    } else {
      header("Connection: close", 17);
    }
  }

  if (arg_vars == nullptr) {
    if (http_data.get_len > 0) {
      array<mixed> argv_array(array_size(1, true));
      argv_array.push_back(get_str);

      superglobals.v$argv = argv_array;
      superglobals.v$argc = 1;
    } else {
      superglobals.v$argv = array<mixed>();
      superglobals.v$argc = 0;
    }
  } else {
    superglobals.v$argc = int64_t{arg_vars->count()};
    superglobals.v$argv = *arg_vars;
  }

  superglobals.v$_SERVER.set_value(string("argv"), superglobals.v$argv);
  superglobals.v$_SERVER.set_value(string("argc"), superglobals.v$argc);

  superglobals.v$d$PHP_SAPI = php_sapi_name();
}

static http_query_data empty_http_data;
static rpc_query_data empty_rpc_data;
static job_query_data empty_job_data;

void init_superglobals(const php_query_data_t& data, PhpScriptBuiltInSuperGlobals& superglobals) {
  // init superglobals depending on the request type
  std::visit(overloaded{[&superglobals](const rpc_query_data& rpc_data) {
                          query_type = QUERY_TYPE_RPC;
                          init_superglobals_impl(empty_http_data, rpc_data, empty_job_data, superglobals);
                        },
                        [&superglobals](const http_query_data& http_data) {
                          query_type = QUERY_TYPE_HTTP;
                          init_superglobals_impl(http_data, empty_rpc_data, empty_job_data, superglobals);
                        },
                        [&superglobals](const job_query_data& job_data) {
                          query_type = QUERY_TYPE_JOB;
                          init_superglobals_impl(empty_http_data, empty_rpc_data, job_data, superglobals);
                        },
                        [&superglobals](const null_query_data&) {
                          query_type = QUERY_TYPE_CONSOLE;
                          init_superglobals_impl(empty_http_data, empty_rpc_data, empty_job_data, superglobals);
                        }},
             data);
}

double f$get_net_time() {
  return get_net_time();
}

double f$get_script_time() {
  return get_script_time();
}

int64_t f$get_net_queries_count() {
  return get_net_queries_count();
}

int64_t f$get_engine_uptime() {
  return get_engine_uptime();
}

string f$get_engine_version() {
  return string(get_engine_version());
}

int64_t f$get_engine_workers_number() {
  return vk::singleton<WorkersControl>::get().get_total_workers_count();
}

string f$get_kphp_cluster_name() {
  return string{vk::singleton<ServerConfig>::get().get_cluster_name()};
}

std::tuple<int64_t, int64_t, int64_t, int64_t> f$get_webserver_stats() {
  const auto& stats = vk::singleton<SharedDataWorkerCache>::get().get_cached_worker_stats();
  return {stats.running_workers, stats.waiting_workers, stats.ready_for_accept_workers, stats.total_workers};
}

static char ini_vars_storage[sizeof(array<string>)];
static array<string>* ini_vars = nullptr;

void ini_set(vk::string_view key, vk::string_view value) {
  php_assert(dl::query_num == 0);

  if (ini_vars == nullptr) {
    new (ini_vars_storage) array<string>();
    ini_vars = reinterpret_cast<array<string>*>(ini_vars_storage);
  }

  ini_vars->set_value(string(key.data(), static_cast<string::size_type>(key.size())), string(value.data(), static_cast<string::size_type>(value.size())));
}

int32_t ini_set_from_config(const char* config_file_name) {
  std::ifstream config(config_file_name);
  if (!config.is_open()) {
    return -1;
  }
  int line_num = 1;
  for (std::string line_; std::getline(config, line_); ++line_num) {
    vk::string_view line = line_;
    auto comment_pos = line.find('#');
    if (comment_pos != std::string::npos) {
      line = line.substr(0, comment_pos);
    }
    line = vk::trim(line);
    if (line.empty()) {
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      return line_num;
    }
    ini_set(line.substr(0, pos), line.substr(pos + 1));
  }
  return 0;
}

Optional<string> f$ini_get(const string& s) {
  if (ini_vars != nullptr && ini_vars->has_key(s)) {
    return ini_vars->get_value(s);
  }

  if (!strcmp(s.c_str(), "sendmail_path")) {
    return string("/usr/sbin/sendmail -ti");
  } else if (!strcmp(s.c_str(), "max_execution_time")) {
    return string(script_timeout);
  } else if (!strcmp(s.c_str(), "memory_limit")) {
    return f$strval((int64_t)dl::get_script_memory_stats().memory_limit); // TODO
  } else if (!strcmp(s.c_str(), "include_path")) {
    return string(); // TODO
  } else if (!strcmp(s.c_str(), "static-buffers-size")) {
    return f$strval(static_cast<int64_t>(static_buffer_length_limit));
  }

  php_warning("Unrecognized option %s in ini_get", s.c_str());
  // php_assert (0);
  return false;
}

bool f$ini_set(const string& s, const string& value) {
  if (!strcmp(s.c_str(), "soap.wsdl_cache_enabled") || !strcmp(s.c_str(), "include_path") || !strcmp(s.c_str(), "memory")) {
    php_warning("Option %s not supported in ini_set", s.c_str());
    return true;
  }
  if (!strcmp(s.c_str(), "default_socket_timeout")) {
    return false; // TODO
  }
  if (!strcmp(s.c_str(), "error_reporting")) {
    return f$error_reporting(f$intval(value));
  }

  php_critical_error("unrecognized option %s in ini_set", s.c_str());
  return false; // unreachable
}

const Stream INPUT("php://input", 11);
const Stream STDIN("php://stdin", 11);
const Stream STDOUT("php://stdout", 12);
const Stream STDERR("php://stderr", 12);

static Stream php_fopen(const string& stream, const string& mode) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    if (!eq2(mode, string("w")) && !eq2(mode, string("a"))) {
      php_warning("%s should be opened in write or append mode", stream.to_string().c_str());
      return false;
    }
    return stream;
  }

  if (eq2(stream, INPUT)) {
    if (!eq2(mode, string("r"))) {
      php_warning("%s should be opened in read mode", stream.to_string().c_str());
      return false;
    }
    return stream;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static Optional<int64_t> php_fwrite(const Stream& stream, const string& text) {
  if (eq2(stream, STDOUT)) {
    print(text);
    return text.size();
  }

  if (eq2(stream, STDERR)) {
    dbg_echo(text);
    return text.size();
  }

  if (eq2(stream, INPUT) || eq2(stream, STDIN)) {
    php_warning("Stream %s is not writeable", stream.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static int64_t php_fseek(const Stream& stream, int64_t offset __attribute__((unused)), int64_t whence __attribute__((unused))) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use fseek with stream %s", stream.to_string().c_str());
    return -1;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use fseek with stream %s", stream.to_string().c_str());
    return -1;
  }

  if (eq2(stream, STDIN)) {
    php_warning("Can't use fseek with stream %s", stream.to_string().c_str());
    return -1;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return -1;
}

static Optional<int64_t> php_ftell(const Stream& stream) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use ftell with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use ftell with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, STDIN)) {
    php_warning("Can't use ftell with stream %s", stream.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static Optional<string> php_fread(const Stream& stream, int64_t length) {
  if (length <= 0) {
    php_warning("Parameter length in function fread must be positive");
    return false;
  }
  if (length > string::max_size()) {
    php_warning("Parameter length in function fread is too large");
    return false;
  }

  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use fread with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use fread with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, STDIN)) {
    string res(static_cast<string::size_type>(length), false);
    dl::enter_critical_section(); // OK
    size_t res_size = fread(&res[0], static_cast<size_t>(length), 1, stdin);
    dl::leave_critical_section();
    php_assert(res_size <= static_cast<size_t>(length));
    res.shrink(static_cast<string::size_type>(res_size));
    return res;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static Optional<string> php_fgetc(const Stream& stream) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use fgetc with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use fgetc with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, STDIN)) {
    dl::enter_critical_section(); // OK
    clearerr(stdin);
    int result = fgetc(stdin);
    if (ferror(stdin)) {
      dl::leave_critical_section();
      php_warning("Error happened during fgetc with stream %s", stream.to_string().c_str());
      return false;
    }
    dl::leave_critical_section();
    if (result == EOF) {
      return false;
    }

    return string(1, static_cast<char>(result));
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static Optional<string> php_fgets(const Stream& stream, int64_t length) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use fgetc with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use fgetc with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, STDIN)) {
    if (length < 0) {
      length = 1024; // TODO remove limit
    }
    if (length > string::max_size()) {
      php_warning("Parameter length in function fgetc is too large");
      return false;
    }
    string res(static_cast<string::size_type>(length), false);
    dl::enter_critical_section(); // OK
    clearerr(stdin);
    char* result = fgets(&res[0], static_cast<int32_t>(length), stdin);
    if (ferror(stdin)) {
      dl::leave_critical_section();
      php_warning("Error happened during fgets with stream %s", stream.to_string().c_str());
      return false;
    }
    dl::leave_critical_section();
    if (result == nullptr) {
      return false;
    }

    res.shrink(static_cast<string::size_type>(strlen(res.c_str())));
    return res;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static Optional<int64_t> php_fpassthru(const Stream& stream) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use fpassthru with stream %s", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, INPUT) || eq2(stream, STDIN)) {
    // TODO implement this
    php_warning("Can't use fpassthru with stream %s", stream.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static bool php_fflush(const Stream& stream) {
  if (eq2(stream, STDOUT)) {
    // TODO implement this
    php_warning("fflush of %s is not implemented yet", stream.to_string().c_str());
    return false;
  }

  if (eq2(stream, STDERR)) {
    return true;
  }

  if (eq2(stream, INPUT) || eq2(stream, STDIN)) {
    php_warning("Stream %s is not writeable, so there is no reason to fflush it", stream.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static bool php_feof(const Stream& stream) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR)) {
    php_warning("Can't use feof with stream %s", stream.to_string().c_str());
    return true;
  }

  if (eq2(stream, INPUT)) {
    // TODO implement this
    php_warning("Can't use feof with stream %s", stream.to_string().c_str());
    return true;
  }

  if (eq2(stream, STDIN)) {
    dl::enter_critical_section(); // OK
    bool eof = (feof(stdin) != 0);
    dl::leave_critical_section();
    return eof;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return true;
}

static Optional<string> php_file_get_contents(const string& url) {
  if (eq2(url, STDOUT) || eq2(url, STDERR)) {
    php_warning("Can't use file_get_contents with stream %s", url.c_str());
    return false;
  }

  if (eq2(url, INPUT)) {
    return raw_post_data;
  }

  if (eq2(url, STDIN)) {
    php_warning("Can't use file_get_contents with stream %s", url.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", url.c_str());
  return false;
}

static Optional<int64_t> php_file_put_contents(const string& url, const string& content, int64_t flags __attribute__((unused))) {
  if (eq2(url, STDOUT)) {
    print(content);
    return content.size();
  }

  if (eq2(url, STDERR)) {
    dbg_echo(content);
    return content.size();
  }

  if (eq2(url, INPUT) || eq2(url, STDIN)) {
    php_warning("Stream %s is not writeable", url.c_str());
    return false;
  }

  php_warning("Stream %s not found", url.c_str());
  return false;
}

static bool php_fclose(const Stream& stream) {
  if (eq2(stream, STDOUT) || eq2(stream, STDERR) || eq2(stream, INPUT)) {
    php_warning("Can't close stream %s", stream.to_string().c_str());
    return false;
  }

  php_warning("Stream %s not found", stream.to_string().c_str());
  return false;
}

static void global_init_interface_lib() {
  static stream_functions php_stream_functions;

  php_stream_functions.name = string("php");
  php_stream_functions.fopen = php_fopen;
  php_stream_functions.fwrite = php_fwrite;
  php_stream_functions.fseek = php_fseek;
  php_stream_functions.ftell = php_ftell;
  php_stream_functions.fread = php_fread;
  php_stream_functions.fgetc = php_fgetc;
  php_stream_functions.fgets = php_fgets;
  php_stream_functions.fpassthru = php_fpassthru;
  php_stream_functions.fflush = php_fflush;
  php_stream_functions.feof = php_feof;
  php_stream_functions.fclose = php_fclose;

  php_stream_functions.file_get_contents = php_file_get_contents;
  php_stream_functions.file_put_contents = php_file_put_contents;

  php_stream_functions.stream_socket_client = nullptr;
  php_stream_functions.context_set_option = nullptr;
  php_stream_functions.stream_set_option = nullptr;
  php_stream_functions.get_fd = nullptr;

  register_stream_functions(&php_stream_functions, false);
}

static void reset_global_interface_vars(PhpScriptBuiltInSuperGlobals& superglobals) {
  dl::enter_critical_section();

  hard_reset_var(http_status_line);

  mixed::reset_empty_values();

  hard_reset_var(superglobals.v$argc);
  hard_reset_var(superglobals.v$argv);

  hard_reset_var(superglobals.v$d$PHP_SAPI);

  hard_reset_var(raw_post_data);

  dl::leave_critical_section();
}

static void init_interface_lib() {
  shutdown_functions_count = 0;
  shutdown_functions_status_value = shutdown_functions_status::not_executed;
  finished = false;

  php_warning_level = std::max(2, php_warning_minimum_level);
  RuntimeContext::get().php_disable_warnings = 0;
  is_json_log_on_timeout_enabled = true;
  is_demangled_stacktrace_logs_enabled = false;
  ignore_level = 0;

  const size_t engine_pid_buf_size = 20;
  static char engine_pid_buf[engine_pid_buf_size];
  dl::enter_critical_section(); // OK
  snprintf(engine_pid_buf, engine_pid_buf_size, "] [%d] ", (int)getpid());
  dl::leave_critical_section();
  engine_pid = engine_pid_buf;

  ob_cur_buffer = -1;
  f$ob_start();

  if (is_utf8_enabled) {
    setlocale(LC_CTYPE, "ru_RU.UTF-8");
  } else {
    setlocale(LC_CTYPE, "ru_RU.CP1251");
  }

  // TODO
  header("HTTP/1.0 200 OK", 15);
  php_assert(http_return_code == 200);
  header("Server: nginx/0.3.33", 20);
  string date = f$gmdate(HTTP_DATE);
  kphp_runtime_context.static_SB_spare.clean() << "Date: " << date;
  header(kphp_runtime_context.static_SB_spare.c_str(), (int)kphp_runtime_context.static_SB_spare.size());
  if (is_utf8_enabled) {
    header("Content-Type: text/html; charset=utf-8", 38);
  } else {
    header("Content-Type: text/html; charset=windows-1251", 45);
  }
}

static void init_runtime_libs() {
  // init_curl_lib() lazy called in runtime
  init_instance_cache_lib();
  init_confdata_functions_lib();
  runtime_builtins_stats::init_request_stats();

  init_memcache_lib();
  init_mysql_lib();
  init_datetime_lib();
  init_net_events_lib();
  init_resumable_lib();
  init_streams_lib();
  init_rpc_lib();
  init_openssl_lib();
  init_math_functions();
  kphp_tracing::init_tracing_lib();
  init_slot_factories();

  init_interface_lib();
}

static void free_shutdown_functions() {
  for (std::size_t i = 0; i < shutdown_functions_count; ++i) {
    shutdown_functions[i].~shutdown_function_type();
  }
  shutdown_functions_count = 0;
}

static void free_header_handler_function() {
  headers_custom_handler_function.~headers_custom_handler_function_type();
  new (&headers_custom_handler_function) headers_custom_handler_function_type{};
  headers_sent = false;
  headers_custom_handler_invoked = false;
}

static void free_interface_lib() {
  dl::enter_critical_section(); // OK
  free_shutdown_functions();
  free_header_handler_function();
  if (dl::query_num == uploaded_files_last_query_num) {
    const array<bool>* const_uploaded_files = uploaded_files;
    for (auto p = const_uploaded_files->begin(); p != const_uploaded_files->end(); ++p) {
      unlink(p.get_key().to_string().c_str());
    }
    uploaded_files_last_query_num--;
  }
  dl::leave_critical_section();
}

static void free_runtime_libs() {
  php_assert(dl::in_critical_section == 0);

  free_interface_lib();
  forcibly_stop_and_flush_profiler();
  free_bcmath_lib();
  free_exception_lib();
  free_curl_lib();
  free_memcache_lib();
  free_mysql_lib();
  free_files_lib();
  free_openssl_lib();
  free_rpc_lib();
  free_typed_rpc_lib();
  free_streams_lib();
  free_udp_lib();
  free_tcp_lib();
  free_timelib();
  OnKphpWarningCallback::get().reset();
  kphp_tracing::free_tracing_lib();
  free_slot_factories();
  runtime_builtins_stats::reset_request_stats();

  free_job_client_interface_lib();
  free_job_server_interface_lib();

  free_confdata_functions_lib();
  free_instance_cache_lib();
  free_kphp_backtrace();

  free_use_updated_gmmktime();

  vk::singleton<JsonLogger>::get().reset_buffers();
#ifdef PDO_DRIVER_MYSQL
  database_drivers::free_mysql_lib();
#endif
#ifdef PDO_DRIVER_PGSQL
  database_drivers::free_pgsql_lib();
#endif
  vk::singleton<database_drivers::Adaptor>::get().reset();
  vk::singleton<curl_async::CurlAdaptor>::get().reset();
  vk::singleton<OomHandler>::get().reset();
  hard_reset_var(SerializationLibContext::get().last_json_processor_error);
}

void global_init_runtime_libs() {
  global_init_profiler();
  global_init_instance_cache_lib();
  global_init_files_lib();
  global_init_interface_lib();
  global_init_openssl_lib();
  global_init_regexp_lib();
  global_init_resumable_lib();
  global_init_rpc_lib();
  global_init_udp_lib();
  global_init_tcp_lib();
  global_init_job_workers_lib();
  global_init_php_timelib();
  global_init_curl_lib();
}

void global_init_script_allocator() {
  dl::global_init_script_allocator();
}

void init_runtime_environment(const php_query_data_t& data, PhpScriptBuiltInSuperGlobals& superglobals, void* mem, size_t script_mem_size,
                              size_t oom_handling_mem_size) {
  runtime_allocator.init(mem, script_mem_size, oom_handling_mem_size);
  kphp_runtime_context.init();
  reset_global_interface_vars(superglobals);
  init_runtime_libs();
  init_superglobals(data, superglobals);
}

void free_runtime_environment(PhpScriptBuiltInSuperGlobals& superglobals) {
  reset_superglobals(superglobals);
  free_runtime_libs();
  reset_global_interface_vars(superglobals);
  kphp_runtime_context.free();
  runtime_allocator.free();
}

void worker_global_init(WorkerType worker_type) noexcept {
  worker_global_init_slot_factories();
  vk::singleton<JsonLogger>::get().reset_json_logs_count();
  worker_global_init_handlers(worker_type);
  init_kphp_ml_runtime_in_worker();
  init_php_scripts_in_each_worker(PhpScriptMutableGlobals::current());
}

void read_engine_tag(const char* file_name) {
  assert(dl::query_num == 0);

  struct stat stat_buf;
  int file_fd = open(file_name, O_RDONLY);
  if (file_fd < 0) {
    assert("Can't open file with engine tag" && 0);
  }
  if (fstat(file_fd, &stat_buf) < 0) {
    assert("Can't do fstat on file with engine tag" && 0);
  }

  const size_t MAX_SIZE = 40;
  char buf[MAX_SIZE + 3];

  size_t size = stat_buf.st_size;
  if (size > MAX_SIZE) {
    size = MAX_SIZE;
  }
  if (read_safe(file_fd, buf, size, {}) < (ssize_t)size) {
    assert("Can't read file with engine tag" && 0);
  }
  close(file_fd);

  for (size_t i = 0; i < size; i++) {
    if (buf[i] < 32 || buf[i] > 126) {
      buf[i] = ' ';
    }
  }

  char prev = ' ';
  size_t j = 0;
  for (size_t i = 0; i < size; i++) {
    if (buf[i] != ' ' || prev != ' ') {
      buf[j++] = buf[i];
    }

    prev = buf[i];
  }
  if (prev == ' ' && j > 0) {
    j--;
  }

  buf[j] = 0;
  ini_set("error_tag", buf);

  buf[j++] = ' ';
  buf[j++] = '[';
  buf[j] = 0;

  engine_tag = strdup(buf);
}

void f$raise_sigsegv() {
  raise(SIGSEGV);
}

void use_utf8() {
  is_utf8_enabled = true;
}

int64_t f$numa_get_bound_node() {
  auto& numa = vk::singleton<NumaConfiguration>::get();
  if (!numa.enabled()) {
    return -1;
  }
  return numa.get_worker_numa_node(logname_id);
}

bool f$extension_loaded(const string& /*extension*/) {
  return true;
}
