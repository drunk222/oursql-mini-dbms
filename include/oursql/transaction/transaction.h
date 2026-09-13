#pragma once

#include "oursql/common/types.h"
#include "oursql/transaction/lock_types.h"

#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace oursql {

using txn_id_t = std::uint64_t;
using lsn_t = std::uint64_t;

class WritePageGuard;
class LockManager;

inline constexpr txn_id_t kInvalidTxnId = 0;
inline constexpr lsn_t kInvalidLsn = 0;

enum class TransactionState {
  Active,
  Failed,
  Committing,
  CommitUncertain,
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

  [[nodiscard]] txn_id_t id() const noexcept;
  [[nodiscard]] TransactionState state() const noexcept;
  [[nodiscard]] lsn_t last_lsn() const noexcept;
  // 调用者：事务协调器/恢复诊断；作用：查询刷盘不确定期间保留的 COMMIT LSN；返回：日志序号。
  [[nodiscard]] lsn_t commit_lsn() const noexcept;
  [[nodiscard]] bool explicit_transaction() const noexcept;
  [[nodiscard]] bool IsUsableForWork() const noexcept;
  [[nodiscard]] std::uint32_t active_write_guards() const noexcept;

  void MarkFailed() noexcept;
  [[nodiscard]] bool TrySetState(TransactionState expected,
                                 TransactionState desired) noexcept;

  [[nodiscard]] std::vector<page_id_t> allocated_pages() const;
  [[nodiscard]] std::vector<page_id_t> deferred_free_pages() const;
  [[nodiscard]] std::vector<page_id_t> AllocatedPagesSnapshot() const;
  [[nodiscard]] std::vector<page_id_t> DeferredFreePagesSnapshot() const;
  [[nodiscard]] bool HasDeferredFreePage(page_id_t page_id) const noexcept;
  [[nodiscard]] std::vector<LockId> GrantedLocksSnapshot() const;

 private:
  friend class TransactionManager;
  friend class BufferPoolManager;
  friend class WritePageGuard;
  friend class LockManager;

  void SetState(TransactionState state) noexcept;
  void SetLastLsn(lsn_t lsn) noexcept;
  void SetCommitLsn(lsn_t lsn) noexcept;
  void AddAllocatedPage(page_id_t page_id);
  void AddDeferredFreePage(page_id_t page_id);
  void AddGrantedLock(const LockId &lock_id);
  void RemoveGrantedLock(const LockId &lock_id);
  void ClearGrantedLocks();
  void AddActiveWriteGuard() noexcept;
  void ReleaseActiveWriteGuard() noexcept;

  txn_id_t id_{kInvalidTxnId};
  TransactionState state_{TransactionState::Active};
  lsn_t last_lsn_{kInvalidLsn};
  lsn_t commit_lsn_{kInvalidLsn};
  bool explicit_transaction_{false};
  std::uint32_t active_write_guards_{0};
  std::vector<page_id_t> allocated_pages_;
  std::vector<page_id_t> deferred_free_pages_;
  std::unordered_set<LockId, LockIdHash> granted_locks_;
  mutable std::mutex mutex_;
};

}  // namespace oursql
