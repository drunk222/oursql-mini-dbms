#pragma once

#include "oursql/execution/executor.h"
#include "oursql/parser/parser.h"
#include "oursql/planner/planner.h"
#include "oursql/recovery/recovery_manager.h"
#include "oursql/storage/log_manager.h"
#include "oursql/transaction/lock_manager.h"
#include "oursql/transaction/transaction_manager.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oursql {

struct DatabaseStatistics {
  std::uint64_t buffer_accesses{0};
  std::uint64_t buffer_hits{0};
  std::uint64_t buffer_misses{0};
  double buffer_hit_rate{0.0};
  std::uint64_t evictions{0};
  std::uint64_t disk_reads{0};
  std::uint64_t disk_writes{0};
};

using session_id_t = std::uint64_t;

// 调用者：DatabaseEngine 和会话客户端；作用：保存一个客户端的执行串行化状态
// 与当前事务；返回：可跨线程传递的会话句柄。current_transaction 只在持有
// execute_mutex 时读写。
struct SessionContext {
  session_id_t session_id{0};
  std::shared_ptr<Transaction> current_transaction;
  // A 层记录已申请的表锁强度，用于安全执行 S->SIX/IX->SIX 等合法升级。
  // 该表只在持有 execute_mutex 时读写，并在事务终结后清空。
  std::unordered_map<table_id_t, TableLockMode> table_lock_modes;
  std::optional<TableLockMode> schema_lock_mode;
  std::mutex execute_mutex;
  std::atomic<bool> closed{false};
};

class DatabaseEngine {
 public:
  // 调用者：命令行或测试；作用：创建数据库编排器并打开指定文件；返回：已初始化的引擎对象。
  explicit DatabaseEngine(std::filesystem::path file_path = "demo.oursql",
                          std::size_t pool_size = 16,
                          ReplacementPolicy replacement_policy = ReplacementPolicy::LRU,
                          FlushPolicy flush_policy = FlushPolicy::WriteBack);
  ~DatabaseEngine();

  DatabaseEngine(const DatabaseEngine &) = delete;
  DatabaseEngine &operator=(const DatabaseEngine &) = delete;

  // 调用者：程序退出流程；作用：刷新缓冲池并关闭数据库文件；返回：成功或错误状态。
  [[nodiscard]] Status Close();
  // 调用者：程序或测试；作用：查询构造期间的打开状态；返回：成功或上下文错误。
  [[nodiscard]] const Status &GetInitStatus() const noexcept;
  // 调用者：CLI 或管理工具；作用：列出已登记表；返回：表元数据副本。
  [[nodiscard]] std::vector<TableMetadata> ListTables() const;
  // 调用者：CLI 或管理工具；作用：读取一张表的元数据；返回：元数据副本或错误。
  [[nodiscard]] Result<TableMetadata> GetTableMetadata(std::string_view table_name) const;
  // 调用者：CLI 或测试；作用：显式写回缓冲池脏页；返回：成功或 I/O 错误。
  [[nodiscard]] Status Flush();
  [[nodiscard]] Status Checkpoint();
  // 调用者：CLI 或答辩基准；作用：读取当前存储统计；返回：统计快照。
  [[nodiscard]] DatabaseStatistics GetStatistics() const;
  // 调用者：CLI 或性能测试；作用：只清零存储统计、不清缓存；返回：成功或引擎状态错误。
  [[nodiscard]] Status ResetStatistics();
  [[nodiscard]] std::vector<FrameSnapshot> GetBufferSnapshots() const;
  [[nodiscard]] std::vector<page_id_t> GetEvictionLog() const;
  void BeginLockEventCapture();
  [[nodiscard]] std::vector<LockEvent> EndLockEventCapture();
  void SetLockEventSession(std::size_t session_index) noexcept;
  void ClearLockEventSession() noexcept;
  [[nodiscard]] ReplacementPolicy GetReplacementPolicy() const noexcept;
  [[nodiscard]] FlushPolicy GetFlushPolicy() const noexcept;
  [[nodiscard]] std::size_t GetPoolSize() const noexcept;
  // 调用者：CLI 或批处理客户端；作用：编译并逐条执行多条 SQL；返回：每条语句的结果序列。
  [[nodiscard]] Result<std::vector<ExecutionResult>> ExecuteSqlBatch(std::string_view sql);
  // 调用者：编译链路展示；作用：对单条 AST 完成语义检查并生成逻辑 Plan；
  // 返回：Plan 或语义/规划错误。该调用不执行 SQL，也不修改 Catalog 或存储。
  [[nodiscard]] Result<Plan> BuildPlan(const Statement &statement) const;
  // 调用者：命令行或客户端；作用：编译并执行多条 SQL；返回：最后一条结果或执行错误。
  [[nodiscard]] Result<ExecutionResult> ExecuteSql(std::string_view sql);
  // 调用者：并发客户端；作用：创建一个独立 Session；返回：会话句柄或引擎错误。
  [[nodiscard]] Result<std::shared_ptr<SessionContext>> CreateSession();
  // 调用者：并发客户端；作用：结束 Session 并回滚/终结其事务；返回：关闭状态。
  [[nodiscard]] Status CloseSession(const std::shared_ptr<SessionContext> &session);
  // 调用者：并发客户端；作用：在指定 Session 内编译并执行多条 SQL；返回：结果序列。
  [[nodiscard]] Result<std::vector<ExecutionResult>> ExecuteSqlBatch(
      const std::shared_ptr<SessionContext> &session, std::string_view sql);
  // 调用者：并发客户端；作用：在指定 Session 内执行 SQL；返回：最后一条结果。
  [[nodiscard]] Result<ExecutionResult> ExecuteSql(
      const std::shared_ptr<SessionContext> &session, std::string_view sql);

 private:
  [[nodiscard]] Result<std::vector<ExecutionResult>> ExecuteSqlBatchLocked(
      const std::shared_ptr<SessionContext> &session, std::string_view sql);
  [[nodiscard]] Status FinishSessionTransaction(
      const std::shared_ptr<SessionContext> &session);
  [[nodiscard]] Status AcquireStatementLocks(
      const std::shared_ptr<SessionContext> &session, const Plan &plan,
      Transaction *transaction);
  [[nodiscard]] bool IsSessionOpen(
      const std::shared_ptr<SessionContext> &session) const;

  struct CachedIndexStatistics {
    std::uint64_t total_entries{0};
    std::unordered_map<std::int64_t, std::uint64_t> frequencies;
  };
  [[nodiscard]] Result<CachedIndexStatistics> GetIndexStatistics(
      std::string_view index_name);
  [[nodiscard]] Plan OptimizeAccessPathByCost(const Plan &plan);
  void InvalidateAccessPathStatistics();

  DiskManager disk_manager_;
  LogManager log_manager_;
  LockManager lock_manager_;
  BufferPoolManager buffer_pool_;
  TransactionManager transaction_manager_;
  RecoveryManager recovery_manager_;
  Catalog catalog_;
  Parser parser_;
  Planner planner_;
  ExecutionEngine execution_engine_;
  Status init_status_;
  std::atomic<bool> closed_{false};
  mutable std::mutex sessions_mutex_;
  mutable std::mutex access_path_statistics_mutex_;
  std::unordered_map<std::string, CachedIndexStatistics>
      access_path_statistics_;
  session_id_t next_session_id_{1};
  std::unordered_map<session_id_t, std::shared_ptr<SessionContext>> sessions_;
  std::shared_ptr<SessionContext> default_session_;
};

}  // namespace oursql
