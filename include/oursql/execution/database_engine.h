#pragma once

#include "oursql/execution/executor.h"
#include "oursql/parser/parser.h"
#include "oursql/planner/planner.h"

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <string_view>
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
  // 调用者：CLI 或答辩基准；作用：读取当前存储统计；返回：统计快照。
  [[nodiscard]] DatabaseStatistics GetStatistics() const;
  // 调用者：命令行或客户端；作用：编译并执行多条 SQL；返回：最后一条结果或执行错误。
  [[nodiscard]] Result<ExecutionResult> ExecuteSql(std::string_view sql);

 private:
  DiskManager disk_manager_;
  BufferPoolManager buffer_pool_;
  Catalog catalog_;
  Parser parser_;
  Planner planner_;
  ExecutionEngine execution_engine_;
  Status init_status_;
  bool closed_{false};
};

}  // namespace oursql
