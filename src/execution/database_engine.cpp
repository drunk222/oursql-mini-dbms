#include "oursql/execution/database_engine.h"

#include <type_traits>
#include <utility>

namespace oursql {

namespace {

Status Contextualize(const char *layer, const Status &status) {
  const auto message = std::string(layer) + ": " + status.message();
  switch (status.code()) {
    case ErrorCode::InvalidArgument: return Status::InvalidArgument(message);
    case ErrorCode::NotFound: return Status::NotFound(message);
    case ErrorCode::NotImplemented: return Status::NotImplemented(message);
    case ErrorCode::AlreadyExists: return Status::AlreadyExists(message);
    case ErrorCode::TypeMismatch: return Status::TypeMismatch(message);
    case ErrorCode::OutOfSpace: return Status::OutOfSpace(message);
    case ErrorCode::RecordTooLarge: return Status::RecordTooLarge(message);
    case ErrorCode::IOError: return Status::IOError(message);
    case ErrorCode::InternalError: return Status::InternalError(message);
    case ErrorCode::Ok: return Status::InternalError(message);
  }
  return Status::InternalError(message);
}

}  // namespace

DatabaseEngine::DatabaseEngine(std::filesystem::path file_path, std::size_t pool_size,
                               ReplacementPolicy replacement_policy, FlushPolicy flush_policy)
    : buffer_pool_(pool_size, &disk_manager_, replacement_policy, flush_policy),
      catalog_(&buffer_pool_, &disk_manager_),
      execution_engine_(&catalog_, &buffer_pool_),
      init_status_(Status::Ok()) {
  init_status_ = buffer_pool_.GetInitStatus();
  if (!init_status_.ok()) return;
  init_status_ = disk_manager_.Open(file_path);
  if (!init_status_.ok()) return;
  init_status_ = catalog_.Open();
}

DatabaseEngine::~DatabaseEngine() {
  (void)Close();
}

Status DatabaseEngine::Close() {
  if (closed_) return Status::Ok();
  if (!init_status_.ok()) {
    closed_ = true;
    return init_status_;
  }
  auto buffer_status = buffer_pool_.Close();
  if (!buffer_status.ok()) return Contextualize("DatabaseEngine", buffer_status);
  auto disk_status = disk_manager_.Close();
  if (!disk_status.ok()) return Contextualize("DatabaseEngine", disk_status);
  closed_ = true;
  return Status::Ok();
}

const Status &DatabaseEngine::GetInitStatus() const noexcept {
  return init_status_;
}

std::vector<TableMetadata> DatabaseEngine::ListTables() const {
  return catalog_.ListTables();
}

Result<TableMetadata> DatabaseEngine::GetTableMetadata(std::string_view table_name) const {
  auto metadata = catalog_.GetTableMetadata(table_name);
  if (!metadata.ok()) return Result<TableMetadata>(metadata.status());
  return Result<TableMetadata>(*metadata.value());
}

Status DatabaseEngine::Flush() {
  if (!init_status_.ok()) return Contextualize("DatabaseEngine", init_status_);
  if (closed_) return Status::InvalidArgument("DatabaseEngine 已关闭");
  return buffer_pool_.FlushAllPages();
}

DatabaseStatistics DatabaseEngine::GetStatistics() const {
  return DatabaseStatistics{buffer_pool_.GetAccessCount(), buffer_pool_.GetHitCount(),
                            buffer_pool_.GetMissCount(), buffer_pool_.GetHitRate(),
                            buffer_pool_.GetEvictionCount(), disk_manager_.GetReadCount(),
                            disk_manager_.GetWriteCount()};
}

Result<std::vector<ExecutionResult>> DatabaseEngine::ExecuteSqlBatch(std::string_view sql) {
  if (!init_status_.ok()) {
    return Result<std::vector<ExecutionResult>>(Contextualize("DatabaseEngine", init_status_));
  }
  if (closed_) {
    return Result<std::vector<ExecutionResult>>(Status::InvalidArgument("DatabaseEngine 已关闭"));
  }

  auto statements = parser_.Parse(sql);
  if (!statements.ok()) {
    return Result<std::vector<ExecutionResult>>(Contextualize("Parser/Lexer", statements.status()));
  }
  std::vector<ExecutionResult> results;
  results.reserve(statements.value().size());
  for (const auto &statement : statements.value()) {
    auto plan = planner_.Build(statement, catalog_);
    if (!plan.ok()) return Result<std::vector<ExecutionResult>>(Contextualize("Planner", plan.status()));
    auto result = execution_engine_.Execute(plan.value());
    if (!result.ok()) return Result<std::vector<ExecutionResult>>(Contextualize("Execution", result.status()));
    results.push_back(std::move(result.value()));
  }
  return Result<std::vector<ExecutionResult>>(std::move(results));
}

Result<ExecutionResult> DatabaseEngine::ExecuteSql(std::string_view sql) {
  auto results = ExecuteSqlBatch(sql);
  if (!results.ok()) return Result<ExecutionResult>(results.status());
  if (results.value().empty()) {
    return Result<ExecutionResult>(Status::InternalError("DatabaseEngine 未产生 SQL 执行结果"));
  }
  return Result<ExecutionResult>(std::move(results.value().back()));
}

}  // namespace oursql
