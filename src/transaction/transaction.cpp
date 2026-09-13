#include "oursql/transaction/transaction.h"

namespace oursql {

txn_id_t Transaction::id() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return id_;
}

TransactionState Transaction::state() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

lsn_t Transaction::last_lsn() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_lsn_;
}

lsn_t Transaction::commit_lsn() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return commit_lsn_;
}

bool Transaction::explicit_transaction() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return explicit_transaction_;
}

bool Transaction::IsUsableForWork() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == TransactionState::Active;
}

std::uint32_t Transaction::active_write_guards() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_write_guards_;
}

void Transaction::MarkFailed() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == TransactionState::Active) state_ = TransactionState::Failed;
}

bool Transaction::TrySetState(TransactionState expected,
                              TransactionState desired) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != expected) return false;
  state_ = desired;
  return true;
}

std::vector<page_id_t> Transaction::allocated_pages() const {
  return AllocatedPagesSnapshot();
}

std::vector<page_id_t> Transaction::deferred_free_pages() const {
  return DeferredFreePagesSnapshot();
}

std::vector<page_id_t> Transaction::AllocatedPagesSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return allocated_pages_;
}

std::vector<page_id_t> Transaction::DeferredFreePagesSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deferred_free_pages_;
}

bool Transaction::HasDeferredFreePage(page_id_t page_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto value : deferred_free_pages_) {
    if (value == page_id) return true;
  }
  return false;
}

std::vector<LockId> Transaction::GrantedLocksSnapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {granted_locks_.begin(), granted_locks_.end()};
}

void Transaction::SetState(TransactionState state) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

void Transaction::SetLastLsn(lsn_t lsn) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  last_lsn_ = lsn;
}

void Transaction::SetCommitLsn(lsn_t lsn) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  commit_lsn_ = lsn;
}

void Transaction::AddAllocatedPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  allocated_pages_.push_back(page_id);
}

void Transaction::AddDeferredFreePage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  deferred_free_pages_.push_back(page_id);
}

void Transaction::AddGrantedLock(const LockId &lock_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.insert(lock_id);
}

void Transaction::RemoveGrantedLock(const LockId &lock_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.erase(lock_id);
}

void Transaction::ClearGrantedLocks() {
  std::lock_guard<std::mutex> lock(mutex_);
  granted_locks_.clear();
}

void Transaction::AddActiveWriteGuard() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  ++active_write_guards_;
}

void Transaction::ReleaseActiveWriteGuard() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_write_guards_ != 0) --active_write_guards_;
}
}
