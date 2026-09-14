#pragma once

#include "oursql/common/status.h"

#include <nlohmann/json.hpp>

#include <mutex>

namespace oursql {

class ConcurrencyDemoService {
 public:
  [[nodiscard]] Result<nlohmann::json> Run();

 private:
  std::mutex run_mutex_;
};

}  // namespace oursql
