#pragma once

#include "oursql/storage/storage.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace oursql {

struct WebServerOptions {
  std::string host{"127.0.0.1"};
  int port{8080};
  std::filesystem::path database_path{"demo.oursql"};
  std::size_t pool_size{16};
  ReplacementPolicy replacement_policy{ReplacementPolicy::LRU};
  FlushPolicy flush_policy{FlushPolicy::WriteBack};
  std::filesystem::path web_root{"web"};
};

inline constexpr std::size_t kMaxWebRequestBodyBytes = 1024U * 1024U;

}  // namespace oursql
