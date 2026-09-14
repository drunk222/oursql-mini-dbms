#include "oursql/service/query_service.h"

#include "oursql/parser/parser.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>

namespace oursql {
namespace {

enum class TransactionStatement { None, Begin, Finish };

TransactionStatement GetTransactionStatement(const Statement &statement) {
  return std::visit(
      [](const auto &value) {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, BeginStatement>) {
          return TransactionStatement::Begin;
        } else if constexpr (std::is_same_v<Type, CommitStatement> ||
                             std::is_same_v<Type, RollbackStatement>) {
          return TransactionStatement::Finish;
        } else {
          return TransactionStatement::None;
        }
      },
      statement);
}

Status ValidateTransactionBatch(const std::vector<Statement> &statements) {
  bool active = false;
  for (const auto &statement : statements) {
    switch (GetTransactionStatement(statement)) {
      case TransactionStatement::None:
        break;
      case TransactionStatement::Begin:
        if (active) {
          return Status::InvalidArgument(
              "a Web request cannot start nested transactions");
        }
        active = true;
        break;
      case TransactionStatement::Finish:
        if (!active) {
          return Status::NotFound(
              "COMMIT or ROLLBACK has no matching BEGIN in this request");
        }
        active = false;
        break;
    }
  }
  if (active) {
    return Status::NotImplemented(
        "cross-request transactions are not supported by the minimal Web demo");
  }
  return Status::Ok();
}

constexpr std::size_t kMaxSqlBytes = 256U * 1024U;

}  // namespace

QueryService::QueryService(DatabaseEngine *engine) noexcept : engine_(engine) {}

Status QueryService::CheckReady() const {
  if (engine_ == nullptr) {
    return Status::InvalidArgument("DatabaseEngine is not available");
  }
  if (!engine_->GetInitStatus().ok()) {
    return engine_->GetInitStatus();
  }
  return Status::Ok();
}

Result<std::vector<ExecutionResult>> QueryService::ExecuteSql(std::string_view sql) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) {
    return Result<std::vector<ExecutionResult>>(std::move(ready));
  }
  if (sql.empty()) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("SQL must not be empty"));
  }
  if (sql.size() > kMaxSqlBytes) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("SQL exceeds the maximum supported length"));
  }

  Parser parser;
  auto statements = parser.Parse(sql);
  if (!statements.ok()) {
    return Result<std::vector<ExecutionResult>>(statements.status());
  }
  const auto transaction_status = ValidateTransactionBatch(statements.value());
  if (!transaction_status.ok()) {
    return Result<std::vector<ExecutionResult>>(transaction_status);
  }
  return engine_->ExecuteSqlBatch(sql);
}

Result<std::vector<TableMetadata>> QueryService::ListTables() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<TableMetadata>>(std::move(ready));
  return Result<std::vector<TableMetadata>>(engine_->ListTables());
}

Result<TableMetadata> QueryService::GetTableMetadata(std::string_view table_name) {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<TableMetadata>(std::move(ready));
  return engine_->GetTableMetadata(table_name);
}

Result<DatabaseStatistics> QueryService::GetStatistics() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<DatabaseStatistics>(std::move(ready));
  return Result<DatabaseStatistics>(engine_->GetStatistics());
}

Result<std::vector<FrameSnapshot>> QueryService::GetBufferSnapshots() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<FrameSnapshot>>(std::move(ready));
  return Result<std::vector<FrameSnapshot>>(engine_->GetBufferSnapshots());
}

Result<std::vector<page_id_t>> QueryService::GetEvictionLog() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return Result<std::vector<page_id_t>>(std::move(ready));
  return Result<std::vector<page_id_t>>(engine_->GetEvictionLog());
}

Status QueryService::Flush() {
  std::lock_guard<std::mutex> lock(execution_mutex_);
  auto ready = CheckReady();
  if (!ready.ok()) return ready;
  return engine_->Flush();
}

}  // namespace oursql
