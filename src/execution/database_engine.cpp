#include "oursql/execution/database_engine.h"

#include "oursql/index/index_manager.h"
#include "oursql/storage/heap_table.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

// 阅读入口：一次 SQL 的固定主路径是
// ExecuteSqlBatch -> Parser::Parse -> Planner::Build -> AcquireStatementLocks
// -> ExecutionEngine::Execute。这里只负责会话和事务编排，不解析 SQL 字符串，
// 也不直接修改 Heap/Index。

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
  if (std::holds_alternative<BeginPlan>(plan)) return TransactionCommand::Begin;
  if (std::holds_alternative<CommitPlan>(plan)) return TransactionCommand::Commit;
  if (std::holds_alternative<RollbackPlan>(plan)) return TransactionCommand::Rollback;
  return TransactionCommand::None;
}

bool IsTransactionStatement(const Statement &statement) {
  return std::holds_alternative<BeginStatement>(statement) ||
         std::holds_alternative<CommitStatement>(statement) ||
         std::holds_alternative<RollbackStatement>(statement);
}

bool IsDdlPlan(const Plan &plan) {
  return std::visit(
      [](const auto &operation) {
        using Type = std::decay_t<decltype(operation)>;
        return std::is_same_v<Type, CreateTablePlan> ||
               std::is_same_v<Type, DropTablePlan> ||
               std::is_same_v<Type, CreateIndexPlan> ||
               std::is_same_v<Type, DropIndexPlan> ||
               std::is_same_v<Type, AlterTablePlan>;
      },
      plan);
}

void MarkFailedIfActive(const std::shared_ptr<Transaction> &transaction) noexcept {
  if (transaction != nullptr && transaction->state() == TransactionState::Active) {
    transaction->MarkFailed();
  }
}

// A 层只把名称映射为进程内锁资源编号。Catalog 尚未持久化稳定 ID，因此该
// 映射不进入磁盘格式；它只保证同一进程中的同名表/索引使用同一把锁。
table_id_t TableIdFor(std::string_view name) noexcept {
  std::uint32_t hash = 2166136261U;
  for (const unsigned char byte : name) {
    hash ^= byte;
    hash *= 16777619U;
  }
  return hash == 0 ? 1U : hash;
}

index_id_t IndexIdFor(std::string_view name) noexcept {
  std::uint32_t hash = 2166136261U;
  for (const unsigned char byte : name) {
    hash ^= byte;
    hash *= 16777619U;
  }
  hash ^= 0x9e3779b9U;
  return hash == 0 ? 1U : hash;
}

std::optional<std::string> EncodeIndexKey(const Value &value) {
  if (value.IsInt()) return "i:" + std::to_string(value.AsInt());
  if (value.IsVarchar()) return "s:" + value.AsVarchar();
  return std::nullopt;
}

std::optional<std::string> PlanTableName(const Plan &plan) {
  return std::visit(
      [](const auto &operation) -> std::optional<std::string> {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, CreateTablePlan> ||
                      std::is_same_v<Type, InsertPlan> ||
                      std::is_same_v<Type, SelectPlan> ||
                      std::is_same_v<Type, DeletePlan> ||
                      std::is_same_v<Type, DropTablePlan> ||
                      std::is_same_v<Type, CreateIndexPlan> ||
                      std::is_same_v<Type, UpdatePlan> ||
                      std::is_same_v<Type, AlterTablePlan>) {
          return operation.table_name;
        } else if constexpr (std::is_same_v<Type, ExplainPlan>) {
          return operation.select.table_name;
        } else {
          return std::nullopt;
        }
      },
      plan);
}

const PlanNode *FindIndexScanNode(const std::shared_ptr<const PlanNode> &node) {
  if (node == nullptr) return nullptr;
  return std::visit(
      [&](const auto &operation) -> const PlanNode * {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, IndexScanPlan>) {
          return node.get();
        } else if constexpr (std::is_same_v<Type, FilterPlan> ||
                             std::is_same_v<Type, ProjectPlan> ||
                             std::is_same_v<Type, GroupByPlan> ||
                             std::is_same_v<Type, OrderByPlan>) {
          return FindIndexScanNode(operation.child);
        } else if constexpr (std::is_same_v<Type, JoinPlan>) {
          const auto *left = FindIndexScanNode(operation.left);
          return left != nullptr ? left : FindIndexScanNode(operation.right);
        } else {
          return nullptr;
        }
      },
      node->operation);
}

std::optional<IndexMetadata> FindIndexForPredicate(
    const Catalog &catalog, std::string_view table_name,
    const Predicate &predicate) {
  if (predicate.kind != PredicateKind::Equal || !predicate.value.IsInt()) {
    return std::nullopt;
  }
  for (const auto &index : catalog.ListTableIndexes(table_name)) {
    if (index.column_name == predicate.column) return index;
  }
  return std::nullopt;
}

Status AcquireSchemaLock(LockManager &lock_manager, SessionContext &session,
                         Transaction *transaction, TableLockMode mode) {
  if (transaction == nullptr) {
    return Status::InvalidArgument("schema lock requires a transaction");
  }
  if (session.schema_lock_mode.has_value()) {
    if (*session.schema_lock_mode == TableLockMode::X ||
        *session.schema_lock_mode == mode) {
      return Status::Ok();
    }
    if (*session.schema_lock_mode != TableLockMode::IS ||
        mode != TableLockMode::X) {
      return Status::InvalidArgument("invalid schema lock upgrade");
    }
  } else if (mode != TableLockMode::IS && mode != TableLockMode::X) {
    return Status::InvalidArgument("schema lock must be IS or X");
  }
  auto status = lock_manager.LockSchema(transaction, mode);
  if (status.ok()) session.schema_lock_mode = mode;
  return status;
}

TableLockMode EffectiveTableMode(std::optional<TableLockMode> current,
                                 TableLockMode requested) noexcept {
  if (!current.has_value()) return requested;
  if (*current == TableLockMode::X || requested == TableLockMode::X) {
    return TableLockMode::X;
  }
  if (*current == TableLockMode::SIX || requested == TableLockMode::SIX) {
    return TableLockMode::SIX;
  }
  if (requested == TableLockMode::IX) {
    return *current == TableLockMode::S ? TableLockMode::SIX
                                        : TableLockMode::IX;
  }
  if (requested == TableLockMode::S) {
    return *current == TableLockMode::IX ? TableLockMode::SIX
                                         : TableLockMode::S;
  }
  if (requested == TableLockMode::IS) {
    if (*current == TableLockMode::IX) return TableLockMode::SIX;
    if (*current == TableLockMode::S || *current == TableLockMode::SIX ||
        *current == TableLockMode::X) {
      return *current;
    }
    return TableLockMode::IS;
  }
  return TableLockMode::SIX;
}

Status AcquireTableLock(SessionContext &session, LockManager &lock_manager,
                        Transaction *transaction, table_id_t table_id,
                        TableLockMode requested) {
  if (transaction == nullptr) {
    return Status::InvalidArgument("table lock requires a transaction");
  }
  const auto current = session.table_lock_modes.find(table_id);
  const TableLockMode effective =
      EffectiveTableMode(current == session.table_lock_modes.end()
                             ? std::nullopt
                             : std::optional<TableLockMode>(current->second),
                         requested);
  auto status = lock_manager.LockTable(transaction, table_id, effective);
  if (status.ok()) session.table_lock_modes[table_id] = effective;
  return status;
}

Status LockPages(LockManager &lock_manager, Transaction *transaction,
                 std::vector<page_id_t> page_ids, LockMode mode) {
  std::sort(page_ids.begin(), page_ids.end());
  page_ids.erase(std::unique(page_ids.begin(), page_ids.end()), page_ids.end());
  for (const auto page_id : page_ids) {
    auto status = lock_manager.LockPage(transaction, page_id, mode);
    if (!status.ok()) return status;
  }
  return Status::Ok();
}

Result<std::vector<RID>> MatchingRids(const Catalog &catalog,
                                      BufferPoolManager &buffer_pool,
                                      std::string_view table_name,
                                      const std::optional<Predicate> &where) {
  auto metadata = catalog.GetTableMetadata(table_name);
  if (!metadata.ok()) {
    return Result<std::vector<RID>>(metadata.status());
  }
  HeapTable table(&buffer_pool, *metadata.value());
  auto rows = table.Scan();
  if (!rows.ok()) return Result<std::vector<RID>>(rows.status());

  std::optional<std::size_t> predicate_index;
  if (where.has_value()) {
    auto index = metadata.value()->schema.FindColumnIndex(where->column);
    if (!index.ok()) return Result<std::vector<RID>>(index.status());
    predicate_index = index.value();
  }

  std::vector<RID> rids;
  for (const auto &entry : rows.value()) {
    if (predicate_index.has_value()) {
      const auto &candidate = entry.second[*predicate_index];
      const auto &predicate = *where;
      const bool matches =
          (predicate.kind == PredicateKind::Equal &&
           candidate == predicate.value) ||
          (predicate.kind == PredicateKind::IsNull && candidate.IsNull()) ||
          (predicate.kind == PredicateKind::IsNotNull && !candidate.IsNull());
      if (!matches) continue;
    }
    rids.push_back(entry.first);
  }
  return Result<std::vector<RID>>(std::move(rids));
}

}  // namespace

DatabaseEngine::DatabaseEngine(std::filesystem::path file_path, std::size_t pool_size,
                               ReplacementPolicy replacement_policy, FlushPolicy flush_policy)
    : log_manager_(),
      lock_manager_(),
      buffer_pool_(pool_size, &disk_manager_, replacement_policy, flush_policy),
      transaction_manager_(&log_manager_, &buffer_pool_, &disk_manager_,
                           &lock_manager_),
      recovery_manager_(&log_manager_, &disk_manager_),
      catalog_(&buffer_pool_, &disk_manager_),
      execution_engine_(&catalog_, &buffer_pool_),
      init_status_(Status::Ok()) {
  default_session_ = std::make_shared<SessionContext>();
  default_session_->session_id = next_session_id_++;
  sessions_.emplace(default_session_->session_id, default_session_);

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
  init_status_ = buffer_pool_.SetLockManager(&lock_manager_);
  if (!init_status_.ok()) return;
  init_status_ = catalog_.Open();
  if (!init_status_.ok()) return;
  init_status_ = transaction_manager_.StartDeadlockDetector();
}

DatabaseEngine::~DatabaseEngine() {
  (void)Close();
}

Status DatabaseEngine::FinishSessionTransaction(
    const std::shared_ptr<SessionContext> &session) {
  if (session == nullptr || session->current_transaction == nullptr) {
    return Status::Ok();
  }
  const auto transaction = session->current_transaction;
  Status status = Status::Ok();
  switch (transaction->state()) {
    case TransactionState::Committed:
    case TransactionState::CommitUncertain:
      status = transaction_manager_.Commit(transaction);
      break;
    case TransactionState::Active:
    case TransactionState::Failed:
      status = transaction_manager_.Abort(transaction);
      break;
    case TransactionState::Aborted:
      status = Status::Ok();
      break;
    case TransactionState::Committing:
    case TransactionState::Aborting:
      status = Status::InvalidArgument(
          "transaction cannot be finalized while another finalizer is active");
      break;
  }
  if (status.ok()) {
    session->current_transaction.reset();
    session->table_lock_modes.clear();
    session->schema_lock_mode.reset();
  }
  return status;
}

Status DatabaseEngine::Close() {
  const auto detector_status = transaction_manager_.StopDeadlockDetector();
  if (!detector_status.ok()) return Contextualize("DatabaseEngine", detector_status);
  if (closed_) return Status::Ok();
  if (!init_status_.ok()) {
    {
      std::lock_guard<std::mutex> lock(sessions_mutex_);
      for (const auto &[id, session] : sessions_) {
        (void)id;
        session->closed.store(true);
      }
    }
    (void)log_manager_.Close();
    (void)disk_manager_.Close();
    closed_ = true;
    return init_status_;
  }

  std::vector<std::shared_ptr<SessionContext>> sessions;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions.reserve(sessions_.size());
    for (const auto &[id, session] : sessions_) {
      (void)id;
      sessions.push_back(session);
    }
  }
  for (const auto &session : sessions) {
    std::lock_guard<std::mutex> execute_lock(session->execute_mutex);
    auto status = FinishSessionTransaction(session);
    if (!status.ok()) return Contextualize("DatabaseEngine", status);
    session->closed.store(true);
  }
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
  }

  auto first_error = Checkpoint();
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

std::size_t DatabaseEngine::GetPoolSize() const noexcept {
  return buffer_pool_.GetPoolSize();
}

Result<std::shared_ptr<SessionContext>> DatabaseEngine::CreateSession() {
  if (!init_status_.ok()) {
    return Result<std::shared_ptr<SessionContext>>(
        Contextualize("DatabaseEngine", init_status_));
  }
  if (closed_) {
    return Result<std::shared_ptr<SessionContext>>(
        Status::InvalidArgument("DatabaseEngine 已关闭"));
  }
  auto session = std::make_shared<SessionContext>();
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  session->session_id = next_session_id_++;
  sessions_.emplace(session->session_id, session);
  return Result<std::shared_ptr<SessionContext>>(std::move(session));
}

bool DatabaseEngine::IsSessionOpen(
    const std::shared_ptr<SessionContext> &session) const {
  if (session == nullptr || session->closed.load()) return false;
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  const auto found = sessions_.find(session->session_id);
  return found != sessions_.end() && found->second == session;
}

Status DatabaseEngine::CloseSession(
    const std::shared_ptr<SessionContext> &session) {
  if (session == nullptr) return Status::InvalidArgument("session is null");
  std::lock_guard<std::mutex> execute_lock(session->execute_mutex);
  if (!IsSessionOpen(session)) return Status::Ok();
  auto status = FinishSessionTransaction(session);
  if (!status.ok()) return status;
  session->closed.store(true);
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  sessions_.erase(session->session_id);
  return Status::Ok();
}

Status DatabaseEngine::AcquireStatementLocks(
    const std::shared_ptr<SessionContext> &session, const Plan &plan,
    Transaction *transaction) {
  if (session == nullptr || transaction == nullptr) {
    return Status::InvalidArgument("statement locks require a session transaction");
  }

  const auto schema_is = [&]() {
    return AcquireSchemaLock(lock_manager_, *session, transaction,
                             TableLockMode::IS);
  };
  const auto table_lock = [&](std::string_view table_name, TableLockMode mode) {
    return AcquireTableLock(*session, lock_manager_, transaction,
                            TableIdFor(table_name), mode);
  };

  if (IsDdlPlan(plan)) {
    auto schema = AcquireSchemaLock(lock_manager_, *session, transaction,
                                    TableLockMode::X);
    if (!schema.ok()) return schema;
    if (const auto *drop_index = std::get_if<DropIndexPlan>(&plan)) {
      auto metadata = catalog_.FindIndex(drop_index->index_name);
      if (!metadata.ok()) return metadata.status();
      return table_lock(metadata.value()->table_name, TableLockMode::X);
    }
    const auto table_name = PlanTableName(plan);
    if (!table_name.has_value()) {
      return Status::InvalidArgument("DDL plan has no target table");
    }
    return table_lock(*table_name, TableLockMode::X);
  }

  if (const auto *insert = std::get_if<InsertPlan>(&plan)) {
    auto schema = schema_is();
    if (!schema.ok()) return schema;
    auto table = table_lock(insert->table_name, TableLockMode::IX);
    if (!table.ok()) return table;
    const auto metadata = catalog_.GetTableMetadata(insert->table_name);
    if (!metadata.ok()) return metadata.status();

    std::set<std::pair<index_id_t, std::string>> key_locks;
    for (const auto &index : catalog_.ListTableIndexes(insert->table_name)) {
      auto column = metadata.value()->schema.FindColumnIndex(index.column_name);
      if (!column.ok()) return column.status();
      for (const auto &row : insert->rows) {
        if (column.value() >= row.size()) {
          return Status::InvalidArgument("INSERT row does not match table schema");
        }
        auto key = EncodeIndexKey(row[column.value()]);
        if (key.has_value()) {
          key_locks.emplace(IndexIdFor(index.name), *key);
        }
      }
    }
    for (const auto &[index_id, key] : key_locks) {
      auto status = lock_manager_.LockIndexKey(
          transaction, index_id, key, LockMode::X, TableIdFor(insert->table_name));
      if (!status.ok()) return status;
    }
    return Status::Ok();
  }

  if (const auto *select = std::get_if<SelectPlan>(&plan)) {
    auto schema = schema_is();
    if (!schema.ok()) return schema;
    const PlanNode *index_node = FindIndexScanNode(select->root);
    if (index_node == nullptr) {
      return table_lock(select->table_name, TableLockMode::S);
    }
    const auto &index_scan = std::get<IndexScanPlan>(index_node->operation);
    auto table = table_lock(index_scan.table_name, TableLockMode::IS);
    if (!table.ok()) return table;
    auto encoded_key = EncodeIndexKey(index_scan.key);
    if (!encoded_key.has_value()) return Status::TypeMismatch("index key has no encoding");
    auto key_status = lock_manager_.LockIndexKey(
        transaction, IndexIdFor(index_scan.index_name), *encoded_key,
        LockMode::S, TableIdFor(index_scan.table_name));
    if (!key_status.ok()) return key_status;

    IndexManager index_manager(&catalog_, &buffer_pool_);
    auto rids = index_manager.Lookup(index_scan.index_name, index_scan.key.AsInt());
    if (!rids.ok()) return rids.status();
    std::vector<page_id_t> pages;
    pages.reserve(rids.value().size());
    for (const auto &rid : rids.value()) pages.push_back(rid.page_id);
    return LockPages(lock_manager_, transaction, std::move(pages), LockMode::S);
  }

  if (const auto *explain = std::get_if<ExplainPlan>(&plan)) {
    auto schema = schema_is();
    if (!schema.ok()) return schema;
    const PlanNode *index_node = FindIndexScanNode(explain->select.root);
    if (index_node == nullptr) {
      return table_lock(explain->select.table_name, TableLockMode::S);
    }
    const auto &index_scan = std::get<IndexScanPlan>(index_node->operation);
    auto table = table_lock(index_scan.table_name, TableLockMode::IS);
    if (!table.ok()) return table;
    auto encoded_key = EncodeIndexKey(index_scan.key);
    if (!encoded_key.has_value()) return Status::TypeMismatch("index key has no encoding");
    return lock_manager_.LockIndexKey(
        transaction, IndexIdFor(index_scan.index_name), *encoded_key,
        LockMode::S, TableIdFor(index_scan.table_name));
  }

  if (const auto *update = std::get_if<UpdatePlan>(&plan)) {
    auto schema = schema_is();
    if (!schema.ok()) return schema;
    std::optional<IndexMetadata> index;
    if (update->where.has_value()) {
      index = FindIndexForPredicate(catalog_, update->table_name, *update->where);
    }
    if (index.has_value() && update->where.has_value()) {
      auto table = table_lock(update->table_name, TableLockMode::IX);
      if (!table.ok()) return table;
      auto encoded_key = EncodeIndexKey(update->where->value);
      if (!encoded_key.has_value()) return Status::TypeMismatch("index key has no encoding");
      auto key_status = lock_manager_.LockIndexKey(
          transaction, IndexIdFor(index->name), *encoded_key, LockMode::X,
          TableIdFor(update->table_name));
      if (!key_status.ok()) return key_status;
      IndexManager index_manager(&catalog_, &buffer_pool_);
      auto rids = index_manager.Lookup(index->name, update->where->value.AsInt());
      if (!rids.ok()) return rids.status();
      std::vector<page_id_t> pages;
      pages.reserve(rids.value().size());
      for (const auto &rid : rids.value()) pages.push_back(rid.page_id);
      return LockPages(lock_manager_, transaction, std::move(pages), LockMode::X);
    }

    auto table = table_lock(update->table_name,
                            update->where.has_value() ? TableLockMode::SIX
                                                      : TableLockMode::X);
    if (!table.ok()) return table;
    if (!update->where.has_value()) return Status::Ok();
    auto rids = MatchingRids(catalog_, buffer_pool_, update->table_name,
                             update->where);
    if (!rids.ok()) return rids.status();
    std::vector<page_id_t> pages;
    pages.reserve(rids.value().size());
    for (const auto &rid : rids.value()) pages.push_back(rid.page_id);
    return LockPages(lock_manager_, transaction, std::move(pages), LockMode::X);
  }

  if (const auto *remove = std::get_if<DeletePlan>(&plan)) {
    auto schema = schema_is();
    if (!schema.ok()) return schema;
    std::optional<IndexMetadata> index;
    if (remove->where.has_value()) {
      index = FindIndexForPredicate(catalog_, remove->table_name, *remove->where);
    }
    if (index.has_value() && remove->where.has_value()) {
      auto table = table_lock(remove->table_name, TableLockMode::IX);
      if (!table.ok()) return table;
      auto encoded_key = EncodeIndexKey(remove->where->value);
      if (!encoded_key.has_value()) return Status::TypeMismatch("index key has no encoding");
      auto key_status = lock_manager_.LockIndexKey(
          transaction, IndexIdFor(index->name), *encoded_key, LockMode::X,
          TableIdFor(remove->table_name));
      if (!key_status.ok()) return key_status;
      IndexManager index_manager(&catalog_, &buffer_pool_);
      auto rids = index_manager.Lookup(index->name, remove->where->value.AsInt());
      if (!rids.ok()) return rids.status();
      std::vector<page_id_t> pages;
      pages.reserve(rids.value().size());
      for (const auto &rid : rids.value()) pages.push_back(rid.page_id);
      return LockPages(lock_manager_, transaction, std::move(pages), LockMode::X);
    }

    auto table = table_lock(remove->table_name,
                            remove->where.has_value() ? TableLockMode::SIX
                                                      : TableLockMode::X);
    if (!table.ok()) return table;
    if (!remove->where.has_value()) return Status::Ok();
    auto rids = MatchingRids(catalog_, buffer_pool_, remove->table_name,
                             remove->where);
    if (!rids.ok()) return rids.status();
    std::vector<page_id_t> pages;
    pages.reserve(rids.value().size());
    for (const auto &rid : rids.value()) pages.push_back(rid.page_id);
    return LockPages(lock_manager_, transaction, std::move(pages), LockMode::X);
  }

  return Status::Ok();
}

Result<std::vector<ExecutionResult>> DatabaseEngine::ExecuteSqlBatchLocked(
    const std::shared_ptr<SessionContext> &session, std::string_view sql) {
  auto statements = parser_.Parse(sql);
  if (!statements.ok()) {
    MarkFailedIfActive(session->current_transaction);
    return Result<std::vector<ExecutionResult>>(
        Contextualize("Parser/Lexer", statements.status()));
  }

  std::vector<ExecutionResult> results;
  results.reserve(statements.value().size());
  for (const auto &statement : statements.value()) {
    const bool transaction_statement = IsTransactionStatement(statement);
    bool autocommit = false;
    if (!transaction_statement) {
      if (session->current_transaction == nullptr) {
        auto begin = transaction_manager_.BeginAutocommit();
        if (!begin.ok()) {
          return Result<std::vector<ExecutionResult>>(
              Contextualize("Transaction", begin.status()));
        }
        session->current_transaction = begin.value();
        session->table_lock_modes.clear();
        session->schema_lock_mode.reset();
        autocommit = true;
      }

      const auto transaction = session->current_transaction;
      const auto state = transaction->state();
      if (state != TransactionState::Active) {
        if (autocommit) (void)FinishSessionTransaction(session);
        if (state == TransactionState::Failed) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "failed transaction only allows ROLLBACK"));
        }
        if (state == TransactionState::Committed ||
            state == TransactionState::CommitUncertain) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "committed transaction cleanup only allows COMMIT retry"));
        }
        return Result<std::vector<ExecutionResult>>(
            Status::InvalidArgument("transaction is not usable"));
      }

      // Catalog reads happen during planning. Schema IS prevents a concurrent
      // DDL from changing the catalog between planning and plan execution.
      auto schema_status =
          AcquireSchemaLock(lock_manager_, *session, transaction.get(),
                            TableLockMode::IS);
      if (!schema_status.ok()) {
        MarkFailedIfActive(transaction);
        const Status failure = Contextualize("SQL lock", schema_status);
        if (autocommit) {
          auto abort = FinishSessionTransaction(session);
          if (!abort.ok()) {
            return Result<std::vector<ExecutionResult>>(
                CombineFailure(failure,
                               Contextualize("Transaction rollback", abort)));
          }
        }
        return Result<std::vector<ExecutionResult>>(failure);
      }
    }

    auto plan = planner_.Build(statement, catalog_);
    if (!plan.ok()) {
      MarkFailedIfActive(session->current_transaction);
      const Status failure = Contextualize("Planner", plan.status());
      if (autocommit) {
        auto abort = FinishSessionTransaction(session);
        if (!abort.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(failure,
                             Contextualize("Transaction rollback", abort)));
        }
      }
      return Result<std::vector<ExecutionResult>>(failure);
    }
    const auto &compiled_plan = plan.value();
    const auto command = GetTransactionCommand(compiled_plan);

    if (command == TransactionCommand::Begin) {
      if (session->current_transaction != nullptr) {
        const auto state = session->current_transaction->state();
        if (state == TransactionState::Failed) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "failed transaction only allows ROLLBACK"));
        }
        if (state == TransactionState::Committed ||
            state == TransactionState::CommitUncertain) {
          return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
              "committed transaction cleanup only allows COMMIT retry"));
        }
        return Result<std::vector<ExecutionResult>>(
            Status::AlreadyExists("session already has an active transaction"));
      }
      auto begin = transaction_manager_.Begin();
      if (!begin.ok()) {
        return Result<std::vector<ExecutionResult>>(
            Contextualize("Transaction", begin.status()));
      }
      session->current_transaction = begin.value();
      session->table_lock_modes.clear();
      session->schema_lock_mode.reset();
      results.push_back(MakeTransactionMessage(
          "Transaction started (txn_id=" +
          std::to_string(begin.value()->id()) + ")"));
      continue;
    }

    if (command == TransactionCommand::Commit) {
      if (session->current_transaction == nullptr) {
        return Result<std::vector<ExecutionResult>>(
            Status::NotFound("no active transaction to commit"));
      }
      const auto transaction = session->current_transaction;
      if (transaction->state() == TransactionState::Failed) {
        return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
            "failed transaction only allows ROLLBACK"));
      }
      auto status = transaction_manager_.Commit(transaction);
      if (!status.ok()) {
        return Result<std::vector<ExecutionResult>>(
            Contextualize("Transaction", status));
      }
      session->current_transaction.reset();
      session->table_lock_modes.clear();
      session->schema_lock_mode.reset();
      results.push_back(MakeTransactionMessage("Transaction committed"));
      continue;
    }

    if (command == TransactionCommand::Rollback) {
      if (session->current_transaction == nullptr) {
        return Result<std::vector<ExecutionResult>>(
            Status::NotFound("no active transaction to roll back"));
      }
      const auto transaction = session->current_transaction;
      if (transaction->state() == TransactionState::Committed ||
          transaction->state() == TransactionState::CommitUncertain) {
        return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
            "committed transaction cleanup only allows COMMIT retry"));
      }
      auto status = transaction_manager_.Abort(transaction);
      if (!status.ok()) {
        return Result<std::vector<ExecutionResult>>(
            Contextualize("Transaction", status));
      }
      session->current_transaction.reset();
      session->table_lock_modes.clear();
      session->schema_lock_mode.reset();
      results.push_back(MakeTransactionMessage("Transaction rolled back"));
      continue;
    }

    const auto transaction = session->current_transaction;
    if (!autocommit && IsDdlPlan(compiled_plan)) {
      MarkFailedIfActive(transaction);
      return Result<std::vector<ExecutionResult>>(Status::InvalidArgument(
          "DDL is not allowed inside an explicit transaction"));
    }

    auto lock_status =
        AcquireStatementLocks(session, compiled_plan, transaction.get());
    if (!lock_status.ok()) {
      MarkFailedIfActive(transaction);
      const Status failure = Contextualize("SQL lock", lock_status);
      if (autocommit) {
        auto abort = FinishSessionTransaction(session);
        if (!abort.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(failure,
                             Contextualize("Transaction rollback", abort)));
        }
      }
      return Result<std::vector<ExecutionResult>>(failure);
    }

    auto result =
        execution_engine_.Execute(compiled_plan, ExecutionContext{transaction.get()});
    if (!result.ok()) {
      MarkFailedIfActive(transaction);
      const Status failure = Contextualize("Execution", result.status());
      if (autocommit) {
        auto abort = FinishSessionTransaction(session);
        if (!abort.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(failure,
                             Contextualize("Transaction rollback", abort)));
        }
      }
      return Result<std::vector<ExecutionResult>>(failure);
    }

    if (!transaction->IsUsableForWork()) {
      const Status failure = Contextualize(
          "Transaction",
          Status::IOError("transaction became failed while recording a page update"));
      if (autocommit) {
        auto abort = FinishSessionTransaction(session);
        if (!abort.ok()) {
          return Result<std::vector<ExecutionResult>>(
              CombineFailure(failure,
                             Contextualize("Transaction rollback", abort)));
        }
      }
      return Result<std::vector<ExecutionResult>>(failure);
    }

    if (autocommit) {
      const auto commit = transaction_manager_.Commit(transaction);
      if (!commit.ok()) {
        const Status failure = Contextualize("Transaction", commit);
        if (transaction->state() != TransactionState::Committed &&
            transaction->state() != TransactionState::CommitUncertain) {
          auto abort = FinishSessionTransaction(session);
          if (!abort.ok()) {
            return Result<std::vector<ExecutionResult>>(
                CombineFailure(failure,
                               Contextualize("Transaction rollback", abort)));
          }
        }
        return Result<std::vector<ExecutionResult>>(failure);
      }
      session->current_transaction.reset();
      session->table_lock_modes.clear();
      session->schema_lock_mode.reset();
    }
    results.push_back(std::move(result.value()));
  }
  return Result<std::vector<ExecutionResult>>(std::move(results));
}

Result<std::vector<ExecutionResult>> DatabaseEngine::ExecuteSqlBatch(
    const std::shared_ptr<SessionContext> &session, std::string_view sql) {
  if (!init_status_.ok()) {
    return Result<std::vector<ExecutionResult>>(
        Contextualize("DatabaseEngine", init_status_));
  }
  if (closed_) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("DatabaseEngine 已关闭"));
  }
  if (!IsSessionOpen(session)) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("session is closed or does not belong to this engine"));
  }
  std::lock_guard<std::mutex> execute_lock(session->execute_mutex);
  if (!IsSessionOpen(session)) {
    return Result<std::vector<ExecutionResult>>(
        Status::InvalidArgument("session is closed"));
  }
  return ExecuteSqlBatchLocked(session, sql);
}

Result<ExecutionResult> DatabaseEngine::ExecuteSql(
    const std::shared_ptr<SessionContext> &session, std::string_view sql) {
  auto results = ExecuteSqlBatch(session, sql);
  if (!results.ok()) return Result<ExecutionResult>(results.status());
  if (results.value().empty()) {
    return Result<ExecutionResult>(
        Status::InternalError("DatabaseEngine 未产生 SQL 执行结果"));
  }
  return Result<ExecutionResult>(std::move(results.value().back()));
}

Result<std::vector<ExecutionResult>> DatabaseEngine::ExecuteSqlBatch(
    std::string_view sql) {
  return ExecuteSqlBatch(default_session_, sql);
}

Result<ExecutionResult> DatabaseEngine::ExecuteSql(std::string_view sql) {
  return ExecuteSql(default_session_, sql);
}

}  // namespace oursql
