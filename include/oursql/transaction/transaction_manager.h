#pragma once

#include "oursql/storage/log_manager.h"
#include "oursql/transaction/transaction.h"

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace oursql {

class BufferPoolManager;
class DiskManager;
class LockManager;

class TransactionManager {
 public:
  TransactionManager(LogManager *log_manager, BufferPoolManager *buffer_pool,
                     DiskManager *disk_manager,
                     LockManager *lock_manager = nullptr) noexcept
      : log_manager_(log_manager),
        buffer_pool_(buffer_pool),
        disk_manager_(disk_manager),
        lock_manager_(lock_manager) {}
  ~TransactionManager();

  TransactionManager(const TransactionManager &) = delete;
  TransactionManager &operator=(const TransactionManager &) = delete;

  // 调用者：DatabaseEngine；作用：启动每 100ms 检查一次死锁的后台线程；返回：启动状态。
  [[nodiscard]] Status StartDeadlockDetector();
  // 调用者：DatabaseEngine/析构函数；作用：唤醒并 join 死锁检测线程；返回：停止状态。
  [[nodiscard]] Status StopDeadlockDetector();

  // Caller: a Session coordinator. Starts one independent transaction.
  [[nodiscard]] Result<std::shared_ptr<Transaction>> Begin();
  // Caller: a Session coordinator. Starts an implicit short transaction.
  [[nodiscard]] Result<std::shared_ptr<Transaction>> BeginAutocommit();
  // Caller: the legacy single-session adapter. Starts a transaction and returns its pointer.
  [[nodiscard]] Result<Transaction *> Begin(bool explicit_transaction);
  // Caller: recovery/bootstrap. Sets the next id only while no transaction is active.
  [[nodiscard]] Status SetNextTxnId(txn_id_t next_id);
  // Caller: the legacy adapter. Commits its compatibility transaction.
  [[nodiscard]] Status Commit();
  // Caller: a Session coordinator. Makes COMMIT durable and finalizes page frees.
  [[nodiscard]] Status Commit(const std::shared_ptr<Transaction> &transaction);
  // Caller: a Session coordinator using a stable raw pointer from Begin(bool).
  [[nodiscard]] Status Commit(Transaction *transaction);
  // Caller: retry path after a durable COMMIT whose page cleanup failed.
  [[nodiscard]] Status FinalizeCommittedCleanup();
  [[nodiscard]] Status FinalizeCommittedCleanup(const std::shared_ptr<Transaction> &transaction);
  // Caller: the legacy adapter. Undoes its compatibility transaction.
  [[nodiscard]] Status Rollback();
  // Caller: a Session/deadlock coordinator. Undoes one transaction and writes ABORT.
  [[nodiscard]] Status Abort(const std::shared_ptr<Transaction> &transaction);
  [[nodiscard]] Status Abort(Transaction *transaction);

  [[nodiscard]] Transaction *Active() const noexcept;
  [[nodiscard]] bool HasActive() const noexcept;
  [[nodiscard]] std::shared_ptr<Transaction> GetTransaction(txn_id_t txn_id) const;
  [[nodiscard]] std::size_t ActiveTransactionCount() const noexcept;
  // Caller: DatabaseEngine checkpoint/close path. Checks every transaction,
  // including transactions created through the multi-session API.
  [[nodiscard]] bool HasAnyTransactionInProgress() const noexcept;
  // Caller: checkpoint coordinator. Rejects checkpoints while any transaction
  // still needs work or cleanup, and includes the blocking transaction.
  [[nodiscard]] Status ValidateCheckpointAllowed() const;
  // Caller: deadlock coordinator. Aborts each detected victim through the
  // normal undo, durable ABORT, and lock-release path.
  [[nodiscard]] Status ResolveDeadlocksOnce();

 private:
  void DeadlockDetectorLoop() noexcept;
  [[nodiscard]] Result<std::shared_ptr<Transaction>> BeginInternal(bool explicit_transaction);
  [[nodiscard]] Result<lsn_t> AppendFor(Transaction *transaction, LogRecord record);
  [[nodiscard]] Status Undo(Transaction *transaction);
  [[nodiscard]] Status ApplyPageImage(page_id_t page_id,
                                       const std::vector<std::byte> &image);
  [[nodiscard]] std::shared_ptr<Transaction> LegacyTransaction() const;
  [[nodiscard]] bool OwnsTransaction(const std::shared_ptr<Transaction> &transaction) const;
  void RemoveTransaction(const std::shared_ptr<Transaction> &transaction);

  LogManager *log_manager_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
  DiskManager *disk_manager_{nullptr};
  LockManager *lock_manager_{nullptr};
  mutable std::mutex mutex_;
  // Protects the legacy single-session Begin(bool) adapter from concurrent
  // nested starts while the multi-session Begin() API remains independent.
  mutable std::mutex legacy_begin_mutex_;
  mutable std::mutex cleanup_mutex_;
  txn_id_t next_txn_id_{1};
  std::unordered_map<txn_id_t, std::shared_ptr<Transaction>> active_transactions_;
  std::unordered_set<txn_id_t> starting_transactions_;
  std::unordered_map<txn_id_t, std::size_t> committed_cleanup_index_;
  std::shared_ptr<Transaction> legacy_active_;

  // Protects detector thread creation/join. It is separate from the wait
  // mutex so Stop can join while the detector is blocked in wait_for.
  mutable std::mutex detector_lifecycle_mutex_;
  mutable std::mutex detector_wait_mutex_;
  std::condition_variable detector_cv_;
  bool detector_stop_requested_{false};
  std::thread detector_thread_;
};

}  // namespace oursql
