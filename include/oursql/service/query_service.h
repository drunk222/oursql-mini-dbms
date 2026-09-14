#pragma once

#include "oursql/execution/database_engine.h"

#include <mutex>
#include <string_view>
#include <vector>

namespace oursql {

struct SqlTraceStep {
  std::string name;
  bool ok{false};
  std::string summary;
  std::vector<std::string> entries;
};

struct SqlTrace {
  std::vector<SqlTraceStep> steps;
};

struct ConcurrentSqlResult {
  Status status;
  std::vector<ExecutionResult> results;
  double duration_ms{0.0};
};

class QueryService {
 public:
  explicit QueryService(DatabaseEngine *engine) noexcept;

  [[nodiscard]] Result<std::vector<ExecutionResult>> ExecuteSql(std::string_view sql);
  [[nodiscard]] Result<SqlTrace> TraceSql(std::string_view sql);
  [[nodiscard]] Result<std::vector<ConcurrentSqlResult>> ExecuteConcurrentSql(
      const std::vector<std::string> &sql_statements);
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
