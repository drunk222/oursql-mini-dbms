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
  // 调用者：DatabaseEngine；作用：绑定 WAL、BufferPool、Disk 和 LockManager；返回：事务协调器对象。
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

  // 调用者：Session 协调器；作用：启动一个独立事务；返回：共享事务对象或错误。
  [[nodiscard]] Result<std::shared_ptr<Transaction>> Begin();
  // 调用者：自动提交执行器；作用：启动一个隐式短事务；返回：共享事务对象或错误。
  [[nodiscard]] Result<std::shared_ptr<Transaction>> BeginAutocommit();
  // 调用者：旧单会话适配器；作用：启动兼容事务并返回稳定指针；返回：事务指针或错误。
  [[nodiscard]] Result<Transaction *> Begin(bool explicit_transaction);
  // 调用者：恢复/启动流程；作用：无活动事务时设置下一个事务号；返回：设置状态。
  [[nodiscard]] Status SetNextTxnId(txn_id_t next_id);
  // 调用者：旧单会话适配器；作用：提交兼容事务；返回：提交状态。
  [[nodiscard]] Status Commit();
  // 调用者：Session 协调器；作用：持久化 COMMIT 并完成待释放页面清理；返回：提交状态。
  [[nodiscard]] Status Commit(const std::shared_ptr<Transaction> &transaction);
  // 调用者：使用 Begin(bool) 指针的旧适配器；作用：提交指定事务；返回：提交状态。
  [[nodiscard]] Status Commit(Transaction *transaction);
  // 调用者：提交后清理重试路径；作用：继续完成已持久化 COMMIT 的页面释放；返回：清理状态。
  [[nodiscard]] Status FinalizeCommittedCleanup();
  [[nodiscard]] Status FinalizeCommittedCleanup(const std::shared_ptr<Transaction> &transaction);
  // 调用者：旧单会话适配器；作用：回滚兼容事务；返回：回滚状态。
  [[nodiscard]] Status Rollback();
  // 调用者：Session/死锁协调器；作用：Undo 指定事务、写入持久化 ABORT 并释放锁；返回：中止状态。
  [[nodiscard]] Status Abort(const std::shared_ptr<Transaction> &transaction);
  [[nodiscard]] Status Abort(Transaction *transaction);

  // 调用者：旧会话适配器；作用：读取兼容事务指针；返回：活动事务或空指针。
  [[nodiscard]] Transaction *Active() const noexcept;
  // 调用者：CLI/关闭流程；作用：判断兼容事务是否存在；返回：判断结果。
  [[nodiscard]] bool HasActive() const noexcept;
  // 调用者：多会话/死锁协调器；作用：按事务号查找活动事务；返回：共享对象或空值。
  [[nodiscard]] std::shared_ptr<Transaction> GetTransaction(txn_id_t txn_id) const;
  // 调用者：诊断/关闭流程；作用：统计活动事务数量；返回：数量。
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
  // 调用者：后台线程；作用：每 100ms 检测并处理死锁 victim；返回：无，错误由协调流程吸收。
  void DeadlockDetectorLoop() noexcept;
  // 调用者：Begin 系列接口；作用：分配事务号、写 Begin WAL 并登记活动事务；返回：事务对象或错误。
  [[nodiscard]] Result<std::shared_ptr<Transaction>> BeginInternal(bool explicit_transaction);
  // 调用者：Commit/Abort；作用：按事务日志链追加记录并更新 last_lsn；返回：新 LSN 或错误。
  [[nodiscard]] Result<lsn_t> AppendFor(Transaction *transaction, LogRecord record);
  // 调用者：Abort/恢复；作用：沿 prev_lsn 逆序撤销页面修改和页面分配；返回：Undo 状态。
  [[nodiscard]] Status Undo(Transaction *transaction);
  // 调用者：Undo；作用：通过缓存池恢复完整页面镜像；返回：写回状态。
  [[nodiscard]] Status ApplyPageImage(page_id_t page_id,
                                       const std::vector<std::byte> &image);
  // 调用者：旧单会话接口；作用：读取 legacy_active_ 的共享副本；返回：事务对象或空值。
  [[nodiscard]] std::shared_ptr<Transaction> LegacyTransaction() const;
  // 调用者：Commit/Abort；作用：确认事务对象属于本管理器；返回：判断结果。
  [[nodiscard]] bool OwnsTransaction(const std::shared_ptr<Transaction> &transaction) const;
  // 调用者：Commit/Abort 完成；作用：从活动表和兼容指针中移除事务；返回：无。
  void RemoveTransaction(const std::shared_ptr<Transaction> &transaction);

  LogManager *log_manager_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
  DiskManager *disk_manager_{nullptr};
  LockManager *lock_manager_{nullptr};
  // 保护事务编号、正在启动/活动事务表、提交后清理进度和 legacy_active_。
  mutable std::mutex mutex_;
  // Protects the legacy single-session Begin(bool) adapter from concurrent
  // nested starts while the multi-session Begin() API remains independent.
  mutable std::mutex legacy_begin_mutex_;
  // 串行化可重试的提交后页面释放，存储 I/O 期间不占用上面的事务表 mutex_。
  mutable std::mutex cleanup_mutex_;
  txn_id_t next_txn_id_{1};
  std::unordered_map<txn_id_t, std::shared_ptr<Transaction>> active_transactions_;
  std::unordered_set<txn_id_t> starting_transactions_;
  std::unordered_map<txn_id_t, std::size_t> committed_cleanup_index_;
  std::shared_ptr<Transaction> legacy_active_;

  // Protects detector thread creation/join. It is separate from the wait
  // mutex so Stop can join while the detector is blocked in wait_for.
  mutable std::mutex detector_lifecycle_mutex_;
  // 只保护停止标志并配合 wait_for；与线程创建/join 的生命周期锁分开。
  mutable std::mutex detector_wait_mutex_;
  std::condition_variable detector_cv_;
  bool detector_stop_requested_{false};
  std::thread detector_thread_;
};

}  // namespace oursql
