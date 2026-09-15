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

  // 调用者：事务/WAL 协调器；作用：读取事务编号；返回：唯一事务号。
  [[nodiscard]] txn_id_t id() const noexcept;
  // 调用者：事务管理器/锁管理器；作用：读取状态机状态；返回：当前状态。
  [[nodiscard]] TransactionState state() const noexcept;
  // 调用者：WAL 追加路径；作用：读取事务日志链尾；返回：最后 LSN。
  [[nodiscard]] lsn_t last_lsn() const noexcept;
  // 调用者：事务协调器/恢复诊断；作用：查询刷盘不确定期间保留的 COMMIT LSN；返回：日志序号。
  [[nodiscard]] lsn_t commit_lsn() const noexcept;
  // 调用者：事务协调器；作用：判断是否由用户显式开启；返回：判断结果。
  [[nodiscard]] bool explicit_transaction() const noexcept;
  // 调用者：执行/锁/缓冲池路径；作用：判断事务能否继续工作；返回：可工作时为 true。
  [[nodiscard]] bool IsUsableForWork() const noexcept;
  // 调用者：提交/关闭流程；作用：查询未释放的写 guard 数；返回：数量。
  [[nodiscard]] std::uint32_t active_write_guards() const noexcept;

  // 调用者：死锁检测或 I/O 失败路径；作用：把事务标记为 Failed；返回：无。
  void MarkFailed() noexcept;
  // 调用者：事务状态机；作用：在期望状态匹配时原子切换到目标状态；返回：是否切换成功。
  [[nodiscard]] bool TrySetState(TransactionState expected,
                                  TransactionState desired) noexcept;

  // 调用者：兼容事务接口；作用：复制事务已分配页面清单；返回：页面编号列表。
  [[nodiscard]] std::vector<page_id_t> allocated_pages() const;
  // 调用者：兼容事务接口；作用：复制提交后待释放页面清单；返回：页面编号列表。
  [[nodiscard]] std::vector<page_id_t> deferred_free_pages() const;
  // 调用者：事务提交/恢复；作用：取得已分配页面的一致快照；返回：页面编号列表。
  [[nodiscard]] std::vector<page_id_t> AllocatedPagesSnapshot() const;
  // 调用者：事务提交/恢复；作用：取得待释放页面的一致快照；返回：页面编号列表。
  [[nodiscard]] std::vector<page_id_t> DeferredFreePagesSnapshot() const;
  // 调用者：提交清理路径；作用：判断某页是否已登记待释放；返回：判断结果。
  [[nodiscard]] bool HasDeferredFreePage(page_id_t page_id) const noexcept;
  // 调用者：锁释放路径；作用：取得事务已授予锁的快照；返回：锁标识列表。
  [[nodiscard]] std::vector<LockId> GrantedLocksSnapshot() const;

 private:
  friend class TransactionManager;
  friend class BufferPoolManager;
  friend class WritePageGuard;
  friend class LockManager;

  // 调用者：TransactionManager；作用：在事务锁保护下写入生命周期状态；返回：无。
  void SetState(TransactionState state) noexcept;
  // 调用者：AppendFor；作用：更新事务日志链尾 LSN；返回：无。
  void SetLastLsn(lsn_t lsn) noexcept;
  // 调用者：Commit；作用：保存已追加的 COMMIT LSN；返回：无。
  void SetCommitLsn(lsn_t lsn) noexcept;
  // 调用者：BufferPool/NewPage；作用：登记事务分配的页面；返回：无。
  void AddAllocatedPage(page_id_t page_id);
  // 调用者：PageFree 日志路径；作用：登记提交后才能释放的页面；返回：无。
  void AddDeferredFreePage(page_id_t page_id);
  // 调用者：LockManager；作用：登记刚授予的资源锁；返回：无。
  void AddGrantedLock(const LockId &lock_id);
  // 调用者：LockManager；作用：移除一个已经释放的资源锁；返回：无。
  void RemoveGrantedLock(const LockId &lock_id);
  // 调用者：LockManager::UnlockAll；作用：清空事务锁清单；返回：无。
  void ClearGrantedLocks();
  // 调用者：WritePageGuard；作用：增加活动写 guard 计数；返回：无。
  void AddActiveWriteGuard() noexcept;
  // 调用者：WritePageGuard::Reset；作用：减少活动写 guard 计数；返回：无。
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
  // 保护单个事务的状态、WAL 链、页面清单、已持有锁集合和活动写 guard 数量。
  // 它不负责阻止两个事务访问同一资源；资源互斥由 LockManager 负责。
  mutable std::mutex mutex_;
};

}  // namespace oursql
