#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"
#include "oursql/storage/heap_table.h"
#include "oursql/storage/storage.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oursql {

struct ExecutionResult {
  std::vector<std::string> column_names;
  std::vector<Row> rows;
  std::vector<RID> rids;
  std::uint64_t affected_rows{0};
};

class RowSource {
 public:
  virtual ~RowSource() = default;

  // 调用者：Filter、Project 或执行引擎；作用：按需取得下一行；返回：行或查询结束标记。
  [[nodiscard]] virtual Result<std::optional<RowEntry>> Next() = 0;
};

class CreateTableExecutor {
 public:
  explicit CreateTableExecutor(Catalog *catalog) noexcept : catalog_(catalog) {}

  // 调用者：ExecutionEngine；作用：创建表并登记持久化元数据；返回：执行结果或错误。
  [[nodiscard]] Result<ExecutionResult> Execute(const CreateTablePlan &plan) const;

 private:
  Catalog *catalog_{nullptr};
};

class InsertExecutor {
 public:
  InsertExecutor(Catalog *catalog, BufferPoolManager *buffer_pool) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool) {}

  // 调用者：ExecutionEngine；作用：按表 Schema 插入一行；返回：影响行数和新 RID。
  [[nodiscard]] Result<ExecutionResult> Execute(const InsertPlan &plan) const;

 private:
  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
};

class SeqScanExecutor {
 public:
  SeqScanExecutor(Catalog *catalog, BufferPoolManager *buffer_pool) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool) {}

  // 调用者：ExecutionEngine；作用：创建一张表的惰性扫描源；返回：逐行源或错误。
  [[nodiscard]] Result<std::unique_ptr<RowSource>> Execute(const SeqScanPlan &plan) const;

 private:
  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
};

class FilterExecutor {
 public:
  // 调用者：ExecutionEngine；作用：包装等值过滤条件；返回：过滤后的逐行源或错误。
  [[nodiscard]] Result<std::unique_ptr<RowSource>> Execute(
      const FilterPlan &plan, std::unique_ptr<RowSource> child, const Schema &schema) const;
};

class ProjectExecutor {
 public:
  // 调用者：ExecutionEngine；作用：包装列投影；返回：投影后的逐行源或错误。
  [[nodiscard]] Result<std::unique_ptr<RowSource>> Execute(
      const ProjectPlan &plan, std::unique_ptr<RowSource> child, const Schema &schema) const;
};

class DeleteExecutor {
 public:
  DeleteExecutor(Catalog *catalog, BufferPoolManager *buffer_pool) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool) {}

  // 调用者：ExecutionEngine；作用：扫描并删除满足条件的行；返回：删除行数或错误。
  [[nodiscard]] Result<ExecutionResult> Execute(const DeletePlan &plan) const;

 private:
  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
};

class ExecutionEngine {
 public:
  ExecutionEngine(Catalog *catalog, BufferPoolManager *buffer_pool) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool) {}

  // 调用者：DatabaseEngine；作用：分派结构化逻辑计划；返回：结果集或影响行数。
  [[nodiscard]] Result<ExecutionResult> Execute(const Plan &plan) const;

 private:
  struct SourcePlan {
    std::unique_ptr<RowSource> source;
    std::vector<std::string> column_names;
  };

  [[nodiscard]] Result<SourcePlan> BuildSource(const SelectPlan &plan) const;

  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
};

}  // namespace oursql
