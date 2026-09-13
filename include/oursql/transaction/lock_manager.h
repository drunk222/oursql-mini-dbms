#pragma once

#include "oursql/common/status.h"
#include "oursql/transaction/lock_types.h"
#include "oursql/transaction/transaction.h"

#include <condition_variable>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oursql {

class LockManager {
 public:
  LockManager() = default;
  ~LockManager() = default;

  LockManager(const LockManager &) = delete;
  LockManager &operator=(const LockManager &) = delete;

  // Caller: transaction coordinator. Acquire a schema lock and wait fairly.
  [[nodiscard]] Status LockSchema(Transaction *transaction, TableLockMode mode);
  // Caller: SQL/catalog coordinator. Acquire a table intention or table lock.
  [[nodiscard]] Status LockTable(Transaction *transaction, table_id_t table_id,
                                 TableLockMode mode);
  // Caller: storage/execution layer. Acquire a page S or X transaction lock.
  [[nodiscard]] Status LockPage(Transaction *transaction, page_id_t page_id,
                                LockMode mode);
  // Caller: index execution layer. Acquire an index-key S or X lock.
  [[nodiscard]] Status LockIndexKey(Transaction *transaction, index_id_t index_id,
                                    const std::string &encoded_key, LockMode mode,
                                    table_id_t table_id = 0);

  // Caller: transaction coordinator. Release one lock after a valid transition.
  [[nodiscard]] Status Unlock(Transaction *transaction, const LockId &lock_id);
  // Caller: transaction coordinator. Release all locks after COMMIT/ABORT.
  [[nodiscard]] Status UnlockAll(Transaction *transaction);

  // Caller: deadlock detector. Mark one youngest cycle victim and wake waiters.
  [[nodiscard]] std::vector<txn_id_t> DetectDeadlocks();
  // Caller: diagnostics/tests. Return a stable snapshot of transaction locks.
  [[nodiscard]] std::vector<LockId> GetGrantedLocks(txn_id_t txn_id) const;
  // Caller: diagnostics/tests. Count currently waiting or upgrading requests.
  [[nodiscard]] std::size_t GetWaitingRequestCount() const;

 private:
  struct LockRequest {
    txn_id_t txn_id{kInvalidTxnId};
    Transaction *transaction{nullptr};
    bool table_mode{false};
    TableLockMode table_lock_mode{TableLockMode::IS};
    LockMode lock_mode{LockMode::S};
    bool granted{false};
    bool upgrading{false};
    bool cancelled{false};
  };

  struct LockRequestQueue {
    std::list<LockRequest> requests;
    std::condition_variable cv;
  };

  using ConstRequestIterator = std::list<LockRequest>::const_iterator;

  [[nodiscard]] Status LockInternal(Transaction *transaction, LockId lock_id,
                                    TableLockMode mode);
  [[nodiscard]] Status LockInternal(Transaction *transaction, LockId lock_id,
                                    LockMode mode);
  [[nodiscard]] static bool IsTableResource(LockResourceType type) noexcept;
  [[nodiscard]] static bool TableModesConflict(TableLockMode left,
                                               TableLockMode right) noexcept;
  [[nodiscard]] static bool LockModesConflict(LockMode left, LockMode right) noexcept;
  [[nodiscard]] static bool CanUpgrade(TableLockMode current,
                                       TableLockMode requested) noexcept;
  [[nodiscard]] static bool CanUpgrade(LockMode current, LockMode requested) noexcept;
  [[nodiscard]] static bool SameRequestedMode(const LockRequest &request,
                                              TableLockMode mode) noexcept;
  [[nodiscard]] static bool SameRequestedMode(const LockRequest &request,
                                              LockMode mode) noexcept;
  [[nodiscard]] bool CanGrant(const LockRequestQueue &queue,
                              ConstRequestIterator request) const noexcept;
  [[nodiscard]] bool RequestConflicts(const LockRequest &request,
                                      const LockRequest &other) const noexcept;
  [[nodiscard]] bool IsWaiting(const LockRequest &request) const noexcept;
  [[nodiscard]] std::optional<txn_id_t> FindDeadlockVictimUnlocked() const;

  mutable std::mutex mutex_;
  std::unordered_map<LockId, LockRequestQueue, LockIdHash> queues_;
  std::unordered_map<txn_id_t, Transaction *> transactions_;
};

}  // namespace oursql
