#pragma once

#include "oursql/storage/log_manager.h"
#include "oursql/transaction/transaction.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace oursql {

class BufferPoolManager;
class DiskManager;

class TransactionManager {
 public:
  TransactionManager(LogManager *log_manager, BufferPoolManager *buffer_pool,
                     DiskManager *disk_manager) noexcept
      : log_manager_(log_manager), buffer_pool_(buffer_pool), disk_manager_(disk_manager) {}

  // Starts the single allowed transaction and returns its stable pointer.
  [[nodiscard]] Result<Transaction *> Begin(bool explicit_transaction);
  // Sets the next transaction id after recovery; only valid while idle.
  [[nodiscard]] Status SetNextTxnId(txn_id_t next_id);
  // Forces COMMIT WAL, then finalizes deferred page frees.
  [[nodiscard]] Status Commit();
  // Retries cleanup after COMMIT is durable without allowing a rollback.
  [[nodiscard]] Status FinalizeCommittedCleanup();
  // Undoes the active transaction from its WAL chain and writes ABORT.
  [[nodiscard]] Status Rollback();

  [[nodiscard]] Transaction *Active() const noexcept { return active_.get(); }
  [[nodiscard]] bool HasActive() const noexcept { return active_ != nullptr; }

 private:
  [[nodiscard]] Result<lsn_t> AppendFor(Transaction *transaction, LogRecord record);
  [[nodiscard]] Status Undo(Transaction *transaction);
  [[nodiscard]] Status ApplyPageImage(page_id_t page_id,
                                       const std::vector<std::byte> &image);

  LogManager *log_manager_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
  DiskManager *disk_manager_{nullptr};
  txn_id_t next_txn_id_{1};
  std::unique_ptr<Transaction> active_;
  std::size_t committed_cleanup_index_{0};
};

}  // namespace oursql
