#include "oursql/transaction/transaction_manager.h"

#include "oursql/storage/storage.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace oursql {

Result<lsn_t> TransactionManager::AppendFor(Transaction *transaction, LogRecord record) {
  if (transaction == nullptr || log_manager_ == nullptr) {
    return Result<lsn_t>(Status::InvalidArgument("TransactionManager is missing transaction or LogManager"));
  }
  record.txn_id = transaction->id();
  record.prev_lsn = transaction->last_lsn();
  auto lsn = log_manager_->Append(std::move(record));
  if (!lsn.ok()) return lsn;
  transaction->SetLastLsn(lsn.value());
  return lsn;
}

Result<Transaction *> TransactionManager::Begin(bool explicit_transaction) {
  if (active_ != nullptr) return Result<Transaction *>(Status::AlreadyExists("an active transaction already exists"));
  if (log_manager_ == nullptr || log_manager_->path().empty()) {
    return Result<Transaction *>(Status::InvalidArgument("TransactionManager is not ready"));
  }
  if (next_txn_id_ == kInvalidTxnId) {
    return Result<Transaction *>(Status::OutOfSpace("no transaction ids remain"));
  }
  const txn_id_t transaction_id = next_txn_id_;
  if (next_txn_id_ == std::numeric_limits<txn_id_t>::max()) {
    next_txn_id_ = kInvalidTxnId;
  } else {
    ++next_txn_id_;
  }
  auto transaction = std::make_unique<Transaction>(transaction_id, explicit_transaction);
  auto begin = AppendFor(transaction.get(), LogRecord{LogRecordType::Begin});
  if (!begin.ok()) {
    next_txn_id_ = transaction_id;
    return Result<Transaction *>(begin.status());
  }
  committed_cleanup_index_ = 0;
  active_ = std::move(transaction);
  return Result<Transaction *>(active_.get());
}

Status TransactionManager::SetNextTxnId(txn_id_t next_id) {
  if (active_ != nullptr) {
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
  std::unordered_map<lsn_t, LogRecord> records;
  for (auto &record : records_result.value()) {
    if (record.txn_id == transaction->id()) records.emplace(record.lsn, std::move(record));
  }
  lsn_t current = transaction->last_lsn();
  while (current != kInvalidLsn) {
    const auto found = records.find(current);
    if (found == records.end()) return Status::IOError("transaction WAL prev_lsn chain is broken");
    const auto &record = found->second;
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
    current = record.prev_lsn;
  }
  return Status::Ok();
}

Status TransactionManager::Commit() {
  if (active_ == nullptr) return Status::NotFound("no active transaction to commit");
  if (active_->state() == TransactionState::Committed) return FinalizeCommittedCleanup();
  if (active_->state() != TransactionState::Active) {
    return Status::InvalidArgument("only an Active transaction can commit");
  }
  if (active_->active_write_guards() != 0) {
    return Status::InvalidArgument("cannot commit while a write page guard is still held");
  }
  active_->SetState(TransactionState::Committing);
  auto commit = AppendFor(active_.get(), LogRecord{LogRecordType::Commit});
  if (!commit.ok()) {
    active_->SetState(TransactionState::Failed);
    return commit.status();
  }
  auto flush = log_manager_->Flush(commit.value());
  if (!flush.ok()) {
    active_->SetState(TransactionState::Failed);
    return Status::IOError("COMMIT WAL was not durable: " + flush.message());
  }
  active_->SetState(TransactionState::Committed);
  committed_cleanup_index_ = 0;
  return FinalizeCommittedCleanup();
}

Status TransactionManager::FinalizeCommittedCleanup() {
  if (active_ == nullptr || active_->state() != TransactionState::Committed) {
    return Status::InvalidArgument("committed cleanup requires a committed transaction");
  }
  const auto &pages = active_->deferred_free_pages();
  while (committed_cleanup_index_ < pages.size()) {
    const page_id_t page_id = pages[committed_cleanup_index_];
    auto status = buffer_pool_ == nullptr ? disk_manager_->DeallocatePage(page_id)
                                          : buffer_pool_->FinalizePageDeletion(page_id);
    if (!status.ok() && status.code() != ErrorCode::AlreadyExists &&
        status.code() != ErrorCode::NotFound) {
      return Status::IOError("transaction committed; cleanup pending for page " +
                             std::to_string(page_id) + ": " + status.message());
    }
    ++committed_cleanup_index_;
  }
  active_.reset();
  committed_cleanup_index_ = 0;
  return Status::Ok();
}

Status TransactionManager::Rollback() {
  if (active_ == nullptr) return Status::NotFound("no active transaction to roll back");
  if (active_->state() == TransactionState::Committed) {
    return Status::InvalidArgument("a committed transaction cannot roll back");
  }
  if (active_->active_write_guards() != 0) {
    return Status::InvalidArgument("cannot roll back while a write page guard is still held");
  }
  active_->SetState(TransactionState::Aborting);
  auto undo = Undo(active_.get());
  if (!undo.ok()) {
    active_->SetState(TransactionState::Failed);
    return Status::IOError("transaction undo failed: " + undo.message());
  }
  auto abort = AppendFor(active_.get(), LogRecord{LogRecordType::Abort});
  if (!abort.ok()) {
    active_->SetState(TransactionState::Failed);
    return abort.status();
  }
  auto flush = log_manager_->Flush(abort.value());
  if (!flush.ok()) {
    active_->SetState(TransactionState::Failed);
    return Status::IOError("ABORT WAL was not durable: " + flush.message());
  }
  active_->SetState(TransactionState::Aborted);
  active_.reset();
  committed_cleanup_index_ = 0;
  return Status::Ok();
}

}  // namespace oursql
