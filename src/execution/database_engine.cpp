#include "oursql/execution/database_engine.h"

#include <type_traits>
#include <utility>

namespace oursql {

namespace {

// 阅读入口：一次 SQL 的固定主路径是
// ExecuteSqlBatch -> Parser::Parse -> Planner::Build -> ExecutionEngine::Execute。
// 调试 CRUD 时先在 ExecuteSqlBatch、再在对应 Executor::Execute 设断点；
// 这里仅负责“编排”，不直接读写表数据。

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

bool IsDmlPlan(const Plan &plan) {
  return std::visit(
      [](const auto &operation) {
        using Type = std::decay_t<decltype(operation)>;
        return std::is_same_v<Type, InsertPlan> || std::is_same_v<Type, DeletePlan> ||
               std::is_same_v<Type, UpdatePlan>;
      },
      plan);
}

bool IsDdlPlan(const Plan &plan) {
  return std::visit(
      [](const auto &operation) {
        using Type = std::decay_t<decltype(operation)>;
        return std::is_same_v<Type, CreateTablePlan> || std::is_same_v<Type, DropTablePlan> ||
               std::is_same_v<Type, CreateIndexPlan> || std::is_same_v<Type, DropIndexPlan> ||
               std::is_same_v<Type, AlterTablePlan>;
      },
      plan);
}

Status RebuildStatus(ErrorCode code, std::string message) {
  switch (code) {
    case ErrorCode::InvalidArgument: return Status::InvalidArgument(std::move(message));
    case ErrorCode::NotFound: return Status::NotFound(std::move(message));
    case ErrorCode::NotImplemented: return Status::NotImplemented(std::move(message));
    case ErrorCode::AlreadyExists: return Status::AlreadyExists(std::move(message));
    case ErrorCode::TypeMismatch: return Status::TypeMismatch(std::move(message));
    case ErrorCode::OutOfSpace: return Status::OutOfSpace(std::move(message));
    case ErrorCode::RecordTooLarge: return Status::RecordTooLarge(std::move(message));
    case ErrorCode::IOError: return Status::IOError(std::move(message));
    case ErrorCode::InternalError: return Status::InternalError(std::move(message));
    case ErrorCode::Ok: return Status::InternalError(std::move(message));
  }
  return Status::InternalError(std::move(message));
}

Status CombineFailure(const Status &original, const Status &rollback) {
  return RebuildStatus(
      original.code(),
      original.message() + "; rollback also failed: " + rollback.message());
}

ExecutionResult MakeTransactionMessage(std::string message) {
  ExecutionResult result;
  result.column_names = {"status"};
  result.rows.push_back({Value(std::move(message))});
  return result;
}

enum class TransactionCommand { None, Begin, Commit, Rollback };

TransactionCommand GetTransactionCommand(const Plan &plan) {
  if (std::holds_alternative<BeginPlan>(plan)) {
    return TransactionCommand::Begin;
  }
  if (std::holds_alternative<CommitPlan>(plan)) {
    return TransactionCommand::Commit;
  }
  if (std::holds_alternative<RollbackPlan>(plan)) {
    return TransactionCommand::Rollback;
  }
  return TransactionCommand::None;
}

}  // namespace

DatabaseEngine::DatabaseEngine(std::filesystem::path file_path, std::size_t pool_size,
                               ReplacementPolicy replacement_policy, FlushPolicy flush_policy)
    : log_manager_(),
      buffer_pool_(pool_size, &disk_manager_, replacement_policy, flush_policy),
      transaction_manager_(&log_manager_, &buffer_pool_, &disk_manager_),
      recovery_manager_(&log_manager_, &disk_manager_),
      catalog_(&buffer_pool_, &disk_manager_),
      execution_engine_(&catalog_, &buffer_pool_),
      init_status_(Status::Ok()) {
  init_status_ = buffer_pool_.GetInitStatus();
  if (!init_status_.ok()) return;
  init_status_ = disk_manager_.Open(file_path);
  if (!init_status_.ok()) return;
  init_status_ = log_manager_.Open(file_path);
  if (!init_status_.ok()) return;
  init_status_ = recovery_manager_.Recover();
  if (!init_status_.ok()) return;
  auto next_txn_id = log_manager_.GetNextTransactionIdSeed();
  if (!next_txn_id.ok()) {
    init_status_ = Contextualize("Transaction recovery", next_txn_id.status());
    return;
  }
  // Recovery has completed all redo/undo writes. A static checkpoint now makes
  // it safe to discard the WAL before the normal Catalog/SQL layers open.
  init_status_ = Checkpoint();
  if (!init_status_.ok()) return;
  init_status_ = transaction_manager_.SetNextTxnId(next_txn_id.value());
  if (!init_status_.ok()) return;
  init_status_ = buffer_pool_.SetLogManager(&log_manager_);
  if (!init_status_.ok()) return;
  init_status_ = catalog_.Open();
}

DatabaseEngine::~DatabaseEngine() {
  (void)Close();
}

Status DatabaseEngine::RollbackAndReloadCatalog() {
  auto rollback = transaction_manager_.Rollback();
  if (!rollback.ok()) return rollback;
  return catalog_.Reload();
}

Status DatabaseEngine::Close() {
  if (closed_) return Status::Ok();
  if (!init_status_.ok()) {
    (void)log_manager_.Close();
    (void)disk_manager_.Close();
    closed_ = true;
    return init_status_;
  }
  Status first_error = Status::Ok();
  if (transaction_manager_.HasActive()) {
    const auto state = transaction_manager_.Active()->state();
    if (state == TransactionState::Committed || state == TransactionState::CommitUncertain) {
      // COMMIT is irreversible. Commit() retries uncertain WAL durability or
      // only retries deferred cleanup for an already committed transaction.
      first_error = transaction_manager_.Commit();
    } else {
      first_error = RollbackAndReloadCatalog();
    }
  }
  if (!first_error.ok()) return Contextualize("DatabaseEngine", first_error);

  first_error = Checkpoint();
  if (!first_error.ok()) return Contextualize("DatabaseEngine", first_error);

  const auto buffer_status = buffer_pool_.Close();
  if (!buffer_status.ok()) return Contextualize("DatabaseEngine", buffer_status);

  const auto disk_status = disk_manager_.Close();
  if (!disk_status.ok()) return Contextualize("DatabaseEngine", disk_status);

  const auto log_status = log_manager_.Close();
  if (!log_status.ok()) return Contextualize("DatabaseEngine", log_status);

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

Status DatabaseEngine::Checkpoint() {
  if (!init_status_.ok()) return Contextualize("DatabaseEngine", init_status_);
  if (closed_) return Status::InvalidArgument("DatabaseEngine is closed");
  const auto transaction_status = transaction_manager_.ValidateCheckpointAllowed();
  if (!transaction_status.ok()) return transaction_status;
  auto status = log_manager_.Flush(log_manager_.GetAppendLsn());
  if (!status.ok()) return status;
  status = buffer_pool_.FlushAllPages();
  if (!status.ok()) return status;
  LogRecord checkpoint;
  checkpoint.type = LogRecordType::Checkpoint;
  auto checkpoint_lsn = log_manager_.Append(std::move(checkpoint));
  if (!checkpoint_lsn.ok()) return checkpoint_lsn.status();
  status = log_manager_.Flush(checkpoint_lsn.value());
  if (!status.ok()) return status;
  return log_manager_.Truncate();
}

DatabaseStatistics DatabaseEngine::GetStatistics() const {
  return DatabaseStatistics{buffer_pool_.GetAccessCount(), buffer_pool_.GetHitCount(),
                            buffer_pool_.GetMissCount(), buffer_pool_.GetHitRate(),
                            buffer_pool_.GetEvictionCount(), disk_manager_.GetReadCount(),
                            disk_manager_.GetWriteCount()};
}

Status DatabaseEngine::ResetStatistics() {
  if (!init_status_.ok()) return Contextualize("DatabaseEngine", init_status_);
  if (closed_) return Status::InvalidArgument("DatabaseEngine is closed");
  buffer_pool_.ResetStatistics();
  disk_manager_.ResetIoStatistics();
  return Status::Ok();
}

std::vector<FrameSnapshot> DatabaseEngine::GetBufferSnapshots() const {
  return buffer_pool_.GetFrameSnapshots();
}

std::vector<page_id_t> DatabaseEngine::GetEvictionLog() const {
  return buffer_pool_.GetEvictionLog();
}

ReplacementPolicy DatabaseEngine::GetReplacementPolicy() const noexcept {
  return buffer_pool_.GetReplacementPolicy();
}

FlushPolicy DatabaseEngine::GetFlushPolicy() const noexcept {
  return buffer_pool_.GetFlushPolicy();
}

std::size_t DatabaseEngine::GetPoolSize() const noexcept { return buffer_pool_.GetPoolSize(); }

Result<std::vector<ExecutionResult>> DatabaseEngine::ExecuteSqlBatch(std::string_view sql) {
  if (!init_status_.ok()) {
    return Result<std::vector<ExecutionResult>>(Contextualize("DatabaseEngine", init_status_));
  }
  if (closed_) {
    return Result<std::vector<ExecutionResult>>(Status::InvalidArgument("DatabaseEngine 已关闭"));
  }

  // 1. 文本 SQL 变成 AST（Statement）。语法不合法会在此返回。
  auto statements = parser_.Parse(sql);
  if (!statements.ok()) {
    if (transaction_manager_.HasActive() &&
        transaction_manager_.Active()->state() == TransactionState::Active) {
      transaction_manager_.Active()->MarkFailed();
    }
    return Result<std::vector<ExecutionResult>>(Contextualize("Parser/Lexer", statements.status()));
  }
  std::vector<ExecutionResult> results;
  results.reserve(statements.value().size());
  for (const auto &statement : statements.value()) {
    // 2. AST 做表/列/类型等语义检查，并变成可执行的 Plan。
    auto plan = planner_.Build(statement, catalog_);
    if (!plan.ok()) {
      if (transaction_manager_.HasActive() &&
          transaction_manager_.Active()->state() == TransactionState::Active) {
        transaction_manager_.Active()->MarkFailed();
      }
      return Result<std::vector<ExecutionResult>>(Contextualize("Planner", plan.status()));
    }
    // 3. Plan 由执行器落实为 Catalog/HeapTable 操作或查询结果。
    const auto &compiled_plan = plan.value();
    const TransactionCommand transaction_command =
        GetTransactionCommand(compiled_plan);
    if (transaction_command != TransactionCommand::None) {
      if (transaction_manager_.HasActive()) {
        const auto state = transaction_manager_.Active()->state();
        if (state == TransactionState::Failed &&
            transaction_command != TransactionCommand::Rollback) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "failed transaction only allows ROLLBACK"));
        }
        if ((state == TransactionState::Committed || state == TransactionState::CommitUncertain) &&
            transaction_command != TransactionCommand::Commit) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "committed transaction cleanup only allows COMMIT retry"));
        }
      }
      Status transaction_status = Status::Ok();
      std::string transaction_message;
      if (transaction_command == TransactionCommand::Begin) {
        auto begin = transaction_manager_.Begin(true);
        if (!begin.ok()) {
          transaction_status = begin.status();
        } else {
          transaction_message = "Transaction started (txn_id=" +
                                std::to_string(begin.value()->id()) + ")";
        }
      } else if (transaction_command == TransactionCommand::Commit) {
        transaction_status = transaction_manager_.Commit();
        if (transaction_status.ok()) {
          transaction_message = "Transaction committed";
        }
      } else {
        transaction_status = RollbackAndReloadCatalog();
        if (transaction_status.ok()) {
          transaction_message = "Transaction rolled back";
        }
      }
      if (!transaction_status.ok()) {
        return Result<std::vector<ExecutionResult>>(
            Contextualize("Transaction", transaction_status));
      }
      results.push_back(MakeTransactionMessage(std::move(transaction_message)));
      continue;
    }
    if (transaction_manager_.HasActive() &&
        transaction_manager_.Active()->state() != TransactionState::Active) {
      return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
          "transaction is not usable; issue ROLLBACK first"));
    }
    if (transaction_manager_.HasActive() && IsDdlPlan(compiled_plan)) {
      transaction_manager_.Active()->MarkFailed();
      return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
          "DDL is not allowed inside an explicit transaction"));
    }
    const bool dml = IsDmlPlan(compiled_plan);
    Transaction *transaction = transaction_manager_.Active();
    bool autocommit = false;
    if (dml && transaction == nullptr) {
      auto begin = transaction_manager_.Begin(false);
      if (!begin.ok()) {
        return Result<std::vector<ExecutionResult>>(Contextualize("Transaction", begin.status()));
      }
      transaction = begin.value();
      autocommit = true;
    }
    auto result = execution_engine_.Execute(compiled_plan, ExecutionContext{transaction});
    if (!result.ok()) {
      if (transaction != nullptr) transaction->MarkFailed();
      Status execution_error = Contextualize("Execution", result.status());
      if (autocommit) {
        auto rollback = RollbackAndReloadCatalog();
        if (!rollback.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(execution_error,
                             Contextualize("Transaction rollback",
                                           rollback)));
        }
      }
      return Result<std::vector<ExecutionResult>>(std::move(execution_error));
    }
    if (transaction != nullptr && !transaction->IsUsableForWork()) {
      const auto transaction_failure = Contextualize(
          "Transaction", Status::IOError(
                             "transaction became failed while recording a page update"));
      if (autocommit) {
        auto rollback = RollbackAndReloadCatalog();
        if (!rollback.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(transaction_failure,
                             Contextualize("Transaction rollback",
                                           rollback)));
        }
      }
      return Result<std::vector<ExecutionResult>>(transaction_failure);
    }
    if (autocommit) {
      auto commit = transaction_manager_.Commit();
      if (!commit.ok()) {
        const Status commit_error = Contextualize("Transaction", commit);
        if (transaction_manager_.HasActive() &&
            transaction_manager_.Active()->state() != TransactionState::Committed &&
            transaction_manager_.Active()->state() != TransactionState::CommitUncertain) {
          auto rollback = RollbackAndReloadCatalog();
          if (!rollback.ok()) {
            return Result<std::vector<ExecutionResult>>(
                CombineFailure(commit_error,
                               Contextualize("Transaction rollback",
                                             rollback)));
          }
        }
        return Result<std::vector<ExecutionResult>>(commit_error);
      }
    }
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
