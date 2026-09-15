#pragma once

#include "oursql/common/status.h"
#include "oursql/transaction/lock_types.h"
#include "oursql/transaction/transaction.h"

#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace oursql {

struct LockEvent {
  double offset_ms{0.0};
  std::size_t session_index{0};
  txn_id_t txn_id{kInvalidTxnId};
  std::string action;
  std::string resource;
  std::string mode;
  table_id_t table_id{0};
  page_id_t page_id{INVALID_PAGE_ID};
  index_id_t index_id{0};
  std::string encoded_key;
  std::string table_name;
  std::string index_name;
};

class LockManager {
 public:
  LockManager() = default;
  ~LockManager() = default;

  LockManager(const LockManager &) = delete;
  LockManager &operator=(const LockManager &) = delete;

  // 调用者：事务协调器；作用：申请 Schema 层锁并公平等待；返回：授予或错误状态。
  [[nodiscard]] Status LockSchema(Transaction *transaction, TableLockMode mode);
  // 调用者：SQL/Catalog 协调器；作用：申请表意向锁或表锁；返回：授予或错误状态。
  [[nodiscard]] Status LockTable(Transaction *transaction, table_id_t table_id,
                                 TableLockMode mode);
  // 调用者：存储/执行层；作用：申请页面 S/X 事务锁；返回：授予或错误状态。
  [[nodiscard]] Status LockPage(Transaction *transaction, page_id_t page_id,
                                LockMode mode);
  // 调用者：索引执行层；作用：申请索引键 S/X 锁；返回：授予或错误状态。
  [[nodiscard]] Status LockIndexKey(Transaction *transaction, index_id_t index_id,
                                    const std::string &encoded_key, LockMode mode,
                                    table_id_t table_id = 0);

  // 调用者：事务协调器；作用：释放一个已授予或待取消的锁请求；返回：释放状态。
  [[nodiscard]] Status Unlock(Transaction *transaction, const LockId &lock_id);
  // 调用者：事务协调器；作用：在 COMMIT/ABORT 后释放事务全部锁；返回：释放状态。
  [[nodiscard]] Status UnlockAll(Transaction *transaction);

  // 调用者：死锁检测器；作用：找出环中最大 txn_id、标记 victim 并唤醒等待者；返回：victim 列表。
  [[nodiscard]] std::vector<txn_id_t> DetectDeadlocks();
  // 调用者：诊断/测试；作用：复制事务已授予锁；返回：稳定锁标识快照。
  [[nodiscard]] std::vector<LockId> GetGrantedLocks(txn_id_t txn_id) const;
  // 调用者：诊断/测试；作用：统计等待或升级中的请求；返回：请求数量。
  [[nodiscard]] std::size_t GetWaitingRequestCount() const;

  // 调用者：并发诊断；作用：开始捕获到 EndEventCapture 为止的锁事件；返回：无。
  void BeginEventCapture();
  // 调用者：并发诊断；作用：停止捕获并取出事件副本；返回：事件列表。
  [[nodiscard]] std::vector<LockEvent> EndEventCapture();
  // 调用者：会话线程；作用：设置当前线程写入诊断事件的会话编号；返回：无。
  static void SetEventSession(std::size_t session_index) noexcept;
  // 调用者：会话线程结束；作用：清除当前线程诊断会话编号；返回：无。
  static void ClearEventSession() noexcept;

 private:
  struct LockRequest {
    // 一条请求既可表示等待中的新锁，也可表示已授予锁或锁升级。
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
    // 同一个 LockId 的请求按进入顺序保存在链表中，cv 唤醒该资源的等待者。
    std::list<LockRequest> requests;
    std::condition_variable cv;
  };

  using ConstRequestIterator = std::list<LockRequest>::const_iterator;

  // 调用者：表锁公开接口；作用：执行表级请求的排队、升级和等待；返回：锁状态。
  [[nodiscard]] Status LockInternal(Transaction *transaction, LockId lock_id,
                                    TableLockMode mode);
  // 调用者：页/索引键公开接口；作用：执行 S/X 请求的排队、升级和等待；返回：锁状态。
  [[nodiscard]] Status LockInternal(Transaction *transaction, LockId lock_id,
                                    LockMode mode);
  // 调用者：内部锁判断；作用：判断资源是否是 Schema/Table；返回：是否使用表锁模式。
  [[nodiscard]] static bool IsTableResource(LockResourceType type) noexcept;
  // 调用者：表锁兼容性检查；作用：判断两个表锁模式是否冲突；返回：冲突结果。
  [[nodiscard]] static bool TableModesConflict(TableLockMode left,
                                               TableLockMode right) noexcept;
  // 调用者：页/键锁兼容性检查；作用：判断两个 S/X 模式是否冲突；返回：冲突结果。
  [[nodiscard]] static bool LockModesConflict(LockMode left, LockMode right) noexcept;
  // 调用者：锁升级路径；作用：判断表锁升级是否合法；返回：允许结果。
  [[nodiscard]] static bool CanUpgrade(TableLockMode current,
                                       TableLockMode requested) noexcept;
  // 调用者：锁升级路径；作用：判断页/键锁升级是否合法；返回：允许结果。
  [[nodiscard]] static bool CanUpgrade(LockMode current, LockMode requested) noexcept;
  // 调用者：重复请求检查；作用：判断表锁请求模式是否相同；返回：判断结果。
  [[nodiscard]] static bool SameRequestedMode(const LockRequest &request,
                                              TableLockMode mode) noexcept;
  // 调用者：重复请求检查；作用：判断页/键锁请求模式是否相同；返回：判断结果。
  [[nodiscard]] static bool SameRequestedMode(const LockRequest &request,
                                              LockMode mode) noexcept;
  // 调用者：LockInternal；作用：执行 FIFO 公平性和模式冲突检查；返回：是否可授予。
  [[nodiscard]] bool CanGrant(const LockRequestQueue &queue,
                              ConstRequestIterator request) const noexcept;
  // 调用者：等待图/CanGrant；作用：判断两条请求是否互相阻塞；返回：冲突结果。
  [[nodiscard]] bool RequestConflicts(const LockRequest &request,
                                      const LockRequest &other) const noexcept;
  // 调用者：等待图；作用：判断请求是否仍在等待或升级；返回：判断结果。
  [[nodiscard]] bool IsWaiting(const LockRequest &request) const noexcept;
  // 调用者：死锁检测；作用：在已持有全局锁时构造等待图并找 victim；返回：可选 victim。
  [[nodiscard]] std::optional<txn_id_t> FindDeadlockVictimUnlocked() const;
  // 调用者：已持有 mutex_ 的锁路径；作用：记录一条可观察锁事件；返回：无。
  void EmitEventUnlocked(std::string action, const LockId &lock_id,
                         std::string mode, txn_id_t txn_id);

  // 一把全局 mutex_ 保护所有资源队列、事务指针表和诊断事件。
  // condition_variable 等待时会暂时释放它，因此其他事务仍可解锁和唤醒等待者。
  mutable std::mutex mutex_;
  std::unordered_map<LockId, LockRequestQueue, LockIdHash> queues_;
  std::unordered_map<txn_id_t, Transaction *> transactions_;
  bool capture_events_{false};
  std::chrono::steady_clock::time_point capture_started_at_;
  std::vector<LockEvent> captured_events_;
};

}  // namespace oursql
