#include "oursql/execution/database_engine.h"
#include "oursql/service/query_service.h"
#include "oursql/web/api_types.h"
#include "oursql/web/web_server.h"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef OURSQL_DEFAULT_WEB_ROOT
#define OURSQL_DEFAULT_WEB_ROOT "web"
#endif

namespace {

void ConfigureConsoleEncoding() {
#ifdef _WIN32
  (void)::SetConsoleOutputCP(CP_UTF8);
  (void)::SetConsoleCP(CP_UTF8);
#endif
}

void PrintUsage() {
  std::cout
      << "Usage: oursql_web [database.oursql] [options]\n"
      << "Options:\n"
      << "  --host HOST                 Default: 127.0.0.1\n"
      << "  --port PORT                 Default: 8080\n"
      << "  --pool-size N               Default: 16\n"
      << "  --policy fifo|lru|clock     Default: lru\n"
      << "  --flush-policy POLICY       write-back or write-through\n"
      << "  --web-root PATH             Static asset directory\n"
      << "  --help                      Show this help\n";
}

bool ParseSize(std::string_view text, std::size_t *value) {
  if (text.empty() || value == nullptr) return false;
  std::size_t parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed == 0) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParsePort(std::string_view text, int *value) {
  if (text.empty() || value == nullptr) return false;
  int parsed = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() || parsed <= 0 ||
      parsed > 65535) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ParsePolicy(std::string_view text, oursql::ReplacementPolicy *policy) {
  if (text == "fifo" || text == "FIFO") {
    *policy = oursql::ReplacementPolicy::FIFO;
    return true;
  }
  if (text == "lru" || text == "LRU") {
    *policy = oursql::ReplacementPolicy::LRU;
    return true;
  }
  if (text == "clock" || text == "CLOCK") {
    *policy = oursql::ReplacementPolicy::CLOCK;
    return true;
  }
  return false;
}

bool ParseFlushPolicy(std::string_view text, oursql::FlushPolicy *policy) {
  if (text == "write-back" || text == "writeback") {
    *policy = oursql::FlushPolicy::WriteBack;
    return true;
  }
  if (text == "write-through" || text == "writethrough") {
    *policy = oursql::FlushPolicy::WriteThrough;
    return true;
  }
  return false;
}

bool ParseOptions(int argc, char **argv, oursql::WebServerOptions *options,
                  bool *help, std::string *error) {
  options->web_root = std::filesystem::u8path(OURSQL_DEFAULT_WEB_ROOT);
  bool database_seen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--help" || argument == "-h") {
      *help = true;
      continue;
    }
    if (argument == "--host") {
      if (++index >= argc || std::string_view(argv[index]).empty()) {
        *error = "--host requires a value";
        return false;
      }
      options->host = argv[index];
      continue;
    }
    if (argument == "--port") {
      if (++index >= argc || !ParsePort(argv[index], &options->port)) {
        *error = "--port requires an integer from 1 to 65535";
        return false;
      }
      continue;
    }
    if (argument == "--pool-size") {
      if (++index >= argc || !ParseSize(argv[index], &options->pool_size)) {
        *error = "--pool-size requires a positive integer";
        return false;
      }
      continue;
    }
    if (argument == "--policy") {
      if (++index >= argc ||
          !ParsePolicy(argv[index], &options->replacement_policy)) {
        *error = "--policy requires fifo, lru, or clock";
        return false;
      }
      continue;
    }
    if (argument == "--flush-policy") {
      if (++index >= argc ||
          !ParseFlushPolicy(argv[index], &options->flush_policy)) {
        *error = "--flush-policy requires write-back or write-through";
        return false;
      }
      continue;
    }
    if (argument == "--web-root") {
      if (++index >= argc || std::string_view(argv[index]).empty()) {
        *error = "--web-root requires a path";
        return false;
      }
      options->web_root = std::filesystem::u8path(argv[index]);
      continue;
    }
    if (argument == "--database") {
      if (++index >= argc || std::string_view(argv[index]).empty()) {
        *error = "--database requires a path";
        return false;
      }
      options->database_path = std::filesystem::u8path(argv[index]);
      database_seen = true;
      continue;
    }
    if (!database_seen && !argument.empty() && argument.front() != '-') {
      options->database_path = std::filesystem::u8path(argv[index]);
      database_seen = true;
      continue;
    }
    *error = "unknown or misplaced argument: " + std::string(argument);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  ConfigureConsoleEncoding();

  oursql::WebServerOptions options;
  bool help = false;
  std::string error;
  if (!ParseOptions(argc, argv, &options, &help, &error) || help) {
    if (!error.empty()) std::cerr << "Error: " << error << '\n';
    PrintUsage();
    return error.empty() ? 0 : 1;
  }

  oursql::DatabaseEngine engine(options.database_path, options.pool_size,
                                options.replacement_policy, options.flush_policy);
  if (!engine.GetInitStatus().ok()) {
    std::cerr << engine.GetInitStatus().ToString() << '\n';
    return 1;
  }

  oursql::QueryService query_service(&engine);
  oursql::WebServer server(options, &query_service);
  const auto initialize = server.Initialize();
  if (!initialize.ok()) {
    std::cerr << initialize.ToString() << '\n';
    return 1;
  }

  std::cout << "OurSQL Web listening on http://" << options.host << ':' << options.port
            << "\n";
  if (!server.Listen()) {
    std::cerr << server.LastError() << '\n';
    return 1;
  }

  const auto close = engine.Close();
  if (!close.ok()) {
    std::cerr << close.ToString() << '\n';
    return 1;
  }
  return 0;
}
