#pragma once

#include "oursql/execution/database_engine.h"

#include <mutex>
#include <string_view>
#include <vector>

namespace oursql {

class QueryService {
 public:
  explicit QueryService(DatabaseEngine *engine) noexcept;

  [[nodiscard]] Result<std::vector<ExecutionResult>> ExecuteSql(std::string_view sql);
  [[nodiscard]] Result<std::vector<TableMetadata>> ListTables();
  [[nodiscard]] Result<TableMetadata> GetTableMetadata(std::string_view table_name);
  [[nodiscard]] Result<DatabaseStatistics> GetStatistics();
  [[nodiscard]] Result<std::vector<FrameSnapshot>> GetBufferSnapshots();
  [[nodiscard]] Result<std::vector<page_id_t>> GetEvictionLog();
  [[nodiscard]] Status Flush();

 private:
  [[nodiscard]] Status CheckReady() const;

  DatabaseEngine *engine_{nullptr};
  mutable std::mutex execution_mutex_;
};

}  // namespace oursql
