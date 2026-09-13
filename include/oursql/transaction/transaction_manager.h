#pragma once

#include "oursql/storage/log_manager.h"
#include "oursql/transaction/transaction.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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

  // Caller: a Session coordinator. Starts one independent transaction.
  [[nodiscard]] Result<std::shared_ptr<Transaction>> Begin();
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
};

}  // namespace oursql
