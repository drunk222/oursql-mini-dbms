#pragma once

#include "oursql/common/status.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace oursql {

// Runs an isolated, reproducible SeqScan/IndexScan comparison for the Web demo.
// Temporary databases keep benchmark data and statistics away from the user's
// currently-open database.
class PerformanceBenchmarkService {
 public:
  [[nodiscard]] Result<nlohmann::json> RunIndexComparison(
      std::size_t row_count, std::size_t repetitions) const;
  [[nodiscard]] Result<nlohmann::json> RunSelectivityComparison(
      std::size_t row_count, std::size_t repetitions) const;
  [[nodiscard]] Result<nlohmann::json> RunBufferComparison(
      const std::string &access_pattern) const;
};

}  // namespace oursql
