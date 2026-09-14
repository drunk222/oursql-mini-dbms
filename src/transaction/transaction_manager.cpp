#include "oursql/transaction/transaction_manager.h"

#include "oursql/storage/storage.h"
#include "oursql/transaction/lock_manager.h"

#include <algorithm>
#include <limits>
#include <queue>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

namespace oursql {

namespace {

const char *TransactionStateName(TransactionState state) noexcept {
  switch (state) {
    case TransactionState::Active: return "Active";
    case TransactionState::Failed: return "Failed";
    case TransactionState::Committing: return "Committing";
    case TransactionState::CommitUncertain: return "CommitUncertain";
    case TransactionState::Committed: return "Committed";
    case TransactionState::Aborting: return "Aborting";
    case TransactionState::Aborted: return "Aborted";
  }
  return "Unknown";
}

}  // namespace

TransactionManager::~TransactionManager() { (void)StopDeadlockDetector(); }

Status TransactionManager::StartDeadlockDetector() {
  std::lock_guard<std::mutex> lifecycle_lock(detector_lifecycle_mutex_);
  if (detector_thread_.joinable()) return Status::Ok();
  {
    std::lock_guard<std::mutex> wait_lock(detector_wait_mutex_);
    detector_stop_requested_ = false;
  }
  try {
    detector_thread_ = std::thread(&TransactionManager::DeadlockDetectorLoop, this);
  } catch (const std::system_error &error) {
    return Status::IOError("cannot start deadlock detector: " + std::string(error.what()));
  }
  return Status::Ok();
}

Status TransactionManager::StopDeadlockDetector() {
  std::lock_guard<std::mutex> lifecycle_lock(detector_lifecycle_mutex_);
  if (!detector_thread_.joinable()) return Status::Ok();
  {
    std::lock_guard<std::mutex> wait_lock(detector_wait_mutex_);
    detector_stop_requested_ = true;
  }
  detector_cv_.notify_all();
  detector_thread_.join();
  return Status::Ok();
}

void TransactionManager::DeadlockDetectorLoop() noexcept {
  while (true) {
    std::unique_lock<std::mutex> wait_lock(detector_wait_mutex_);
    const bool stop_requested = detector_cv_.wait_for(
        wait_lock, std::chrono::milliseconds(100),
        [this] { return detector_stop_requested_; });
    wait_lock.unlock();
    if (stop_requested) return;
    (void)ResolveDeadlocksOnce();
  }
}

Result<lsn_t> TransactionManager::AppendFor(Transaction *transaction, LogRecord record) {
  if (transaction == nullptr || log_manager_ == nullptr) {
    return Result<lsn_t>(Status::InvalidArgument(
        "TransactionManager is missing transaction or LogManager"));
  }

  // Serialize only this transaction's log chain. LogManager serializes the
  // global LSN and physical append operation independently.
  std::lock_guard<std::mutex> transaction_lock(transaction->mutex_);
  record.txn_id = transaction->id_;
  record.prev_lsn = transaction->last_lsn_;
  auto lsn = log_manager_->Append(std::move(record));
  if (!lsn.ok()) return lsn;
  transaction->last_lsn_ = lsn.value();
  return lsn;
}

Result<std::shared_ptr<Transaction>> TransactionManager::BeginInternal(
    bool explicit_transaction) {
  if (log_manager_ == nullptr || log_manager_->path().empty()) {
    return Result<std::shared_ptr<Transaction>>(
        Status::InvalidArgument("TransactionManager is not ready"));
  }

  txn_id_t transaction_id = kInvalidTxnId;
  std::shared_ptr<Transaction> transaction;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_txn_id_ == kInvalidTxnId) {
      return Result<std::shared_ptr<Transaction>>(
          Status::OutOfSpace("no transaction ids remain"));
    }
    transaction_id = next_txn_id_;
    if (next_txn_id_ == std::numeric_limits<txn_id_t>::max()) {
      next_txn_id_ = kInvalidTxnId;
    } else {
      ++next_txn_id_;
    }
    starting_transactions_.insert(transaction_id);
    transaction = std::make_shared<Transaction>(transaction_id, explicit_transaction);
  }

  LogRecord begin_record;
  begin_record.type = LogRecordType::Begin;
  auto begin = AppendFor(transaction.get(), std::move(begin_record));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    starting_transactions_.erase(transaction_id);
    if (begin.ok()) active_transactions_.emplace(transaction_id, transaction);
  }
  if (!begin.ok()) return Result<std::shared_ptr<Transaction>>(begin.status());
  return Result<std::shared_ptr<Transaction>>(std::move(transaction));
}

Result<std::shared_ptr<Transaction>> TransactionManager::Begin() {
  return BeginInternal(true);
}

Result<std::shared_ptr<Transaction>> TransactionManager::BeginAutocommit() {
  return BeginInternal(false);
}

Result<Transaction *> TransactionManager::Begin(bool explicit_transaction) {
  // This overload is retained for legacy raw-pointer callers. Do not create
  // another transaction while that adapter still owns one.
  std::lock_guard<std::mutex> legacy_begin_lock(legacy_begin_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (legacy_active_ != nullptr) {
      return Result<Transaction *>(
          Status::AlreadyExists("legacy session already has an active transaction"));
    }
  }
  auto transaction = BeginInternal(explicit_transaction);
  if (!transaction.ok()) return Result<Transaction *>(transaction.status());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (legacy_active_ == nullptr) legacy_active_ = transaction.value();
  }
  return Result<Transaction *>(transaction.value().get());
}

Status TransactionManager::SetNextTxnId(txn_id_t next_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_transactions_.empty() || !starting_transactions_.empty()) {
    return Status::InvalidArgument("cannot reset transaction id while a transaction is active");
  }
  if (next_id == kInvalidTxnId) {
    return Status::OutOfSpace("transaction id seed is exhausted");
  }
  next_txn_id_ = next_id;
  return Status::Ok();
}

Status TransactionManager::ApplyPageImage(page_id_t page_id,
                                           const std::vector<std::byte> &image) {
  if (disk_manager_ == nullptr || image.size() != Page::kSize) {
    return Status::InvalidArgument("transaction undo requires one full page image");
  }
  Page page;
  std::copy(image.begin(), image.end(), page.Data());
  if (buffer_pool_ != nullptr) return buffer_pool_->RestorePage(page_id, page, true);
  return disk_manager_->WritePage(page_id, page);
}

Status TransactionManager::Undo(Transaction *transaction) {
  if (transaction == nullptr || log_manager_ == nullptr) {
    return Status::InvalidArgument("TransactionManager undo is not ready");
  }
  auto records_result = log_manager_->Scan();
  if (!records_result.ok()) return records_result.status();

  std::unordered_map<lsn_t, const LogRecord *> records;
  for (const auto &record : records_result.value()) {
    if (record.txn_id == transaction->id()) records.emplace(record.lsn, &record);
  }

  std::priority_queue<lsn_t> pending;
  std::unordered_set<lsn_t> visited;
  const auto last_lsn = transaction->last_lsn();
  if (last_lsn != kInvalidLsn) pending.push(last_lsn);
  while (!pending.empty()) {
    const auto current = pending.top();
    pending.pop();
    if (!visited.insert(current).second) continue;
    const auto found = records.find(current);
    if (found == records.end()) {
      return Status::IOError("transaction WAL prev_lsn chain is broken");
    }
    const auto &record = *found->second;
    if (record.type == LogRecordType::PageUpdate) {
      auto status = ApplyPageImage(record.page_id, record.before_image);
      if (!status.ok()) return status;
    } else if (record.type == LogRecordType::PageAllocate) {
      auto status = buffer_pool_ == nullptr ? disk_manager_->DeallocatePage(record.page_id)
                                            : buffer_pool_->DeletePage(record.page_id);
      if (!status.ok() && status.code() != ErrorCode::AlreadyExists) return status;
    } else if (record.type == LogRecordType::PageFree && buffer_pool_ != nullptr) {
      auto status = buffer_pool_->CancelPageDeletion(record.page_id);
      if (!status.ok() && status.code() != ErrorCode::NotFound) return status;
    }
    if (record.prev_lsn != kInvalidLsn) pending.push(record.prev_lsn);
  }
  return Status::Ok();
}

std::shared_ptr<Transaction> TransactionManager::LegacyTransaction() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return legacy_active_;
}

bool TransactionManager::OwnsTransaction(
    const std::shared_ptr<Transaction> &transaction) const {
  if (transaction == nullptr) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_transactions_.find(transaction->id());
  return found != active_transactions_.end() && found->second == transaction;
}

void TransactionManager::RemoveTransaction(
    const std::shared_ptr<Transaction> &transaction) {
  if (transaction == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = transaction->id();
  const auto found = active_transactions_.find(id);
  if (found != active_transactions_.end() && found->second == transaction) {
    active_transactions_.erase(found);
  }
  committed_cleanup_index_.erase(id);
  if (legacy_active_ == transaction) legacy_active_.reset();
}

Status TransactionManager::Commit() {
  const auto transaction = LegacyTransaction();
  if (transaction == nullptr) return Status::NotFound("no active transaction to commit");
  return Commit(transaction);
}

Status TransactionManager::Commit(const std::shared_ptr<Transaction> &transaction) {
  if (!OwnsTransaction(transaction)) {
    return Status::NotFound("transaction is not active in this manager");
  }
  const auto current = transaction->state();
  if (current == TransactionState::Committed) return FinalizeCommittedCleanup(transaction);
  if (current == TransactionState::CommitUncertain) {
    const auto commit_lsn = transaction->commit_lsn();
    if (commit_lsn == kInvalidLsn) {
      return Status::InternalError("CommitUncertain transaction has no commit LSN");
    }
    auto flush = log_manager_->Flush(commit_lsn);
    if (!flush.ok()) {
      return Status::IOError("COMMIT WAL durability is still uncertain: " + flush.message());
    }
    transaction->SetState(TransactionState::Committed);
    return FinalizeCommittedCleanup(transaction);
  }
  if (current == TransactionState::Failed) {
    return Status::InvalidArgument("a Failed transaction must be aborted");
  }
  if (current != TransactionState::Active ||
      !transaction->TrySetState(TransactionState::Active, TransactionState::Committing)) {
    return Status::InvalidArgument("only one Active transaction can commit");
  }
  if (transaction->active_write_guards() != 0) {
    transaction->SetState(TransactionState::Active);
    return Status::InvalidArgument("cannot commit while a write page guard is still held");
  }

  LogRecord commit_record;
  commit_record.type = LogRecordType::Commit;
  auto commit = AppendFor(transaction.get(), std::move(commit_record));
  if (!commit.ok()) {
    transaction->SetState(TransactionState::Failed);
    return commit.status();
  }
  transaction->SetCommitLsn(commit.value());
  auto flush = log_manager_->Flush(commit.value());
  if (!flush.ok()) {
    transaction->SetState(TransactionState::CommitUncertain);
    return Status::IOError("COMMIT WAL durability is uncertain; retry COMMIT: " +
                           flush.message());
  }
  transaction->SetState(TransactionState::Committed);
  return FinalizeCommittedCleanup(transaction);
}

Status TransactionManager::Commit(Transaction *transaction) {
  if (transaction == nullptr) return Status::InvalidArgument("commit requires a Transaction");
  const auto shared = GetTransaction(transaction->id());
  if (shared == nullptr || shared.get() != transaction) {
    return Status::NotFound("transaction is not active in this manager");
  }
  return Commit(shared);
}

Status TransactionManager::FinalizeCommittedCleanup() {
  const auto transaction = LegacyTransaction();
  if (transaction == nullptr) {
    return Status::NotFound("no committed transaction cleanup is pending");
  }
  return FinalizeCommittedCleanup(transaction);
}

Status TransactionManager::FinalizeCommittedCleanup(
    const std::shared_ptr<Transaction> &transaction) {
  if (!OwnsTransaction(transaction) || transaction->state() != TransactionState::Committed) {
    return Status::InvalidArgument("committed cleanup requires a committed transaction");
  }
  // Cleanup may be retried by a second caller after a durable COMMIT. Keep
  // the retry position and page release operation single-threaded without
  // holding the transaction table mutex across storage I/O.
  std::lock_guard<std::mutex> cleanup_lock(cleanup_mutex_);
  if (!OwnsTransaction(transaction)) {
    return Status::NotFound("transaction cleanup is no longer pending");
  }
  const auto pages = transaction->DeferredFreePagesSnapshot();
  std::size_t position = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    position = committed_cleanup_index_[transaction->id()];
  }
  while (position < pages.size()) {
    const auto page_id = pages[position];
    auto status = buffer_pool_ == nullptr ? disk_manager_->DeallocatePage(page_id)
                                          : buffer_pool_->FinalizePageDeletion(page_id);
    if (!status.ok() && status.code() != ErrorCode::AlreadyExists &&
        status.code() != ErrorCode::NotFound) {
      return Status::IOError("transaction committed; cleanup pending for page " +
                             std::to_string(page_id) + ": " + status.message());
    }
    ++position;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      committed_cleanup_index_[transaction->id()] = position;
    }
  }
  if (lock_manager_ != nullptr) {
    auto unlock = lock_manager_->UnlockAll(transaction.get());
    if (!unlock.ok()) return unlock;
  }
  RemoveTransaction(transaction);
  return Status::Ok();
}

Status TransactionManager::Rollback() {
  const auto transaction = LegacyTransaction();
  if (transaction == nullptr) return Status::NotFound("no active transaction to roll back");
  return Abort(transaction);
}

Status TransactionManager::Abort(const std::shared_ptr<Transaction> &transaction) {
  if (!OwnsTransaction(transaction)) {
    return Status::NotFound("transaction is not active in this manager");
  }
  const auto current = transaction->state();
  if (current == TransactionState::Committed || current == TransactionState::CommitUncertain) {
    return Status::InvalidArgument(
        "a committed or CommitUncertain transaction cannot roll back");
  }
  if (current == TransactionState::Aborted) {
    return Status::InvalidArgument("transaction has already been aborted");
  }
  if (transaction->active_write_guards() != 0) {
    return Status::InvalidArgument("cannot roll back while a write page guard is still held");
  }
  if (current == TransactionState::Committing || current == TransactionState::Aborting ||
      (!transaction->TrySetState(current, TransactionState::Aborting))) {
    return Status::InvalidArgument("transaction is already being finalized");
  }

  auto undo = Undo(transaction.get());
  if (!undo.ok()) {
    transaction->SetState(TransactionState::Failed);
    return Status::IOError("transaction undo failed: " + undo.message());
  }
  LogRecord abort_record;
  abort_record.type = LogRecordType::Abort;
  auto abort = AppendFor(transaction.get(), std::move(abort_record));
  if (!abort.ok()) {
    transaction->SetState(TransactionState::Failed);
    return abort.status();
  }
  auto flush = log_manager_->Flush(abort.value());
  if (!flush.ok()) {
    transaction->SetState(TransactionState::Failed);
    return Status::IOError("ABORT WAL was not durable: " + flush.message());
  }
  transaction->SetState(TransactionState::Aborted);
  if (lock_manager_ != nullptr) {
    auto unlock = lock_manager_->UnlockAll(transaction.get());
    if (!unlock.ok()) return unlock;
  }
  RemoveTransaction(transaction);
  return Status::Ok();
}

Status TransactionManager::Abort(Transaction *transaction) {
  if (transaction == nullptr) return Status::InvalidArgument("abort requires a Transaction");
  const auto shared = GetTransaction(transaction->id());
  if (shared == nullptr || shared.get() != transaction) {
    return Status::NotFound("transaction is not active in this manager");
  }
  return Abort(shared);
}

Transaction *TransactionManager::Active() const noexcept {
  const auto transaction = LegacyTransaction();
  return transaction.get();
}

bool TransactionManager::HasActive() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return legacy_active_ != nullptr;
}

std::shared_ptr<Transaction> TransactionManager::GetTransaction(txn_id_t txn_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_transactions_.find(txn_id);
  return found == active_transactions_.end() ? nullptr : found->second;
}

std::size_t TransactionManager::ActiveTransactionCount() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_transactions_.size() + starting_transactions_.size();
}

bool TransactionManager::HasAnyTransactionInProgress() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return !starting_transactions_.empty() || !active_transactions_.empty() ||
         !committed_cleanup_index_.empty();
}

Status TransactionManager::ValidateCheckpointAllowed() const {
  std::shared_ptr<Transaction> blocking_transaction;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!starting_transactions_.empty()) {
      return Status::InvalidArgument("checkpoint requires no transaction starting (txn_id=" +
                                     std::to_string(*starting_transactions_.begin()) + ")");
    }
    if (!committed_cleanup_index_.empty()) {
      return Status::InvalidArgument(
          "checkpoint requires committed transaction cleanup to finish (txn_id=" +
          std::to_string(committed_cleanup_index_.begin()->first) + ")");
    }
    if (!active_transactions_.empty()) blocking_transaction = active_transactions_.begin()->second;
  }
  if (blocking_transaction != nullptr) {
    return Status::InvalidArgument(
        "checkpoint requires no transaction in progress (txn_id=" +
        std::to_string(blocking_transaction->id()) + ", state=" +
        TransactionStateName(blocking_transaction->state()) + ")");
  }
  return Status::Ok();
}

Status TransactionManager::ResolveDeadlocksOnce() {
  if (lock_manager_ == nullptr) {
    return Status::InvalidArgument("deadlock resolution requires a LockManager");
  }
  const auto victims = lock_manager_->DetectDeadlocks();
  for (const auto txn_id : victims) {
    const auto transaction = GetTransaction(txn_id);
    if (transaction == nullptr) {
      return Status::NotFound("deadlock victim is not managed by TransactionManager (txn_id=" +
                              std::to_string(txn_id) + ")");
    }
    const auto status = Abort(transaction);
    if (!status.ok()) {
      return Status::IOError("deadlock victim abort failed (txn_id=" +
                             std::to_string(txn_id) + "): " + status.message());
    }
  }
  return Status::Ok();
}

}  // namespace oursql
