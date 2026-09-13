#pragma once

#include "oursql/common/types.h"

#include <cstdint>
#include <vector>

namespace oursql {

using txn_id_t = std::uint64_t;
using lsn_t = std::uint64_t;

class WritePageGuard;

inline constexpr txn_id_t kInvalidTxnId = 0;
inline constexpr lsn_t kInvalidLsn = 0;

enum class TransactionState {
  Active,
  Failed,
  Committing,
  Committed,
  Aborting,
  Aborted,
};

// ExecutionContext is passed from the session coordinator to DML executors.
struct ExecutionContext {
  class Transaction *transaction{nullptr};
};

// A transaction stores state and page bookkeeping, not a copy of table data.
class Transaction {
 public:
  Transaction(txn_id_t id, bool explicit_transaction) noexcept
      : id_(id), explicit_transaction_(explicit_transaction) {}

  [[nodiscard]] txn_id_t id() const noexcept { return id_; }
  [[nodiscard]] TransactionState state() const noexcept { return state_; }
  [[nodiscard]] lsn_t last_lsn() const noexcept { return last_lsn_; }
  [[nodiscard]] bool explicit_transaction() const noexcept {
    return explicit_transaction_;
  }
  [[nodiscard]] bool IsUsableForWork() const noexcept {
    return state_ == TransactionState::Active;
  }
  [[nodiscard]] std::uint32_t active_write_guards() const noexcept {
    return active_write_guards_;
  }

  void MarkFailed() noexcept {
    if (state_ == TransactionState::Active) state_ = TransactionState::Failed;
  }

  [[nodiscard]] const std::vector<page_id_t> &allocated_pages() const noexcept {
    return allocated_pages_;
  }
  [[nodiscard]] const std::vector<page_id_t> &deferred_free_pages() const noexcept {
    return deferred_free_pages_;
  }

 private:
  friend class TransactionManager;
  friend class BufferPoolManager;
  friend class WritePageGuard;

  void SetState(TransactionState state) noexcept { state_ = state; }
  void SetLastLsn(lsn_t lsn) noexcept { last_lsn_ = lsn; }
  void AddAllocatedPage(page_id_t page_id) { allocated_pages_.push_back(page_id); }
  void AddDeferredFreePage(page_id_t page_id) {
    deferred_free_pages_.push_back(page_id);
  }
  void AddActiveWriteGuard() noexcept { ++active_write_guards_; }
  void ReleaseActiveWriteGuard() noexcept {
    if (active_write_guards_ != 0) --active_write_guards_;
  }

  txn_id_t id_{kInvalidTxnId};
  TransactionState state_{TransactionState::Active};
  lsn_t last_lsn_{kInvalidLsn};
  bool explicit_transaction_{false};
  std::uint32_t active_write_guards_{0};
  std::vector<page_id_t> allocated_pages_;
  std::vector<page_id_t> deferred_free_pages_;
};

}  // namespace oursql
