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

Result<ExecutionResult> DatabaseEngine::ExecuteSql(std::string_view sql) {
  if (!init_status_.ok()) {
    return Result<ExecutionResult>(Contextualize("DatabaseEngine", init_status_));
  }
  if (closed_) {
    return Result<ExecutionResult>(Status::InvalidArgument("DatabaseEngine 已关闭"));
  }

  auto statements = parser_.Parse(sql);
  if (!statements.ok()) {
    return Result<ExecutionResult>(Contextualize("Parser/Lexer", statements.status()));
  }
  ExecutionResult last_result;
  for (const auto &statement : statements.value()) {
    auto plan = planner_.Build(statement, catalog_);
    if (!plan.ok()) return Result<ExecutionResult>(Contextualize("Planner", plan.status()));
    auto result = execution_engine_.Execute(plan.value());
    if (!result.ok()) return Result<ExecutionResult>(Contextualize("Execution", result.status()));
    last_result = std::move(result.value());
  }
  return Result<ExecutionResult>(std::move(last_result));
}

}  // namespace oursql
