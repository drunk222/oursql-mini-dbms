#include "oursql/execution/executor.h"

#include "oursql/index/index_manager.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

// 本文件是数据库功能层的核心：Plan 在这里被映射为表操作。
// 推荐断点顺序：ExecutionEngine::Execute -> 对应 XxxExecutor::Execute
// -> HeapTable::{InsertRow, BeginScan, DeleteRow}。不要先进入 BufferPoolManager。

Status Contextualize(const char *layer, const Status &status) {
  // 保留原错误码，只为消息增加执行层上下文，便于定位失败发生在哪个算子。
  const auto message = std::string(layer) + ": " + status.message();
  switch (status.code()) {
    case ErrorCode::InvalidArgument: return Status::InvalidArgument(message);
    case ErrorCode::NotFound: return Status::NotFound(message);
    case ErrorCode::NotImplemented: return Status::NotImplemented(message);
    case ErrorCode::AlreadyExists: return Status::AlreadyExists(message);
    case ErrorCode::TypeMismatch: return Status::TypeMismatch(message);
    case ErrorCode::OutOfSpace: return Status::OutOfSpace(message);
    case ErrorCode::RecordTooLarge: return Status::RecordTooLarge(message);
    case ErrorCode::IOError: return Status::IOError(message);
    case ErrorCode::InternalError: return Status::InternalError(message);
    case ErrorCode::Ok: return Status::InternalError(message);
  }
  return Status::InternalError(message);
}

class HeapTableSource final : public RowSource {
 public:
  explicit HeapTableSource(HeapTable::ScanCursor cursor) noexcept : cursor_(std::move(cursor)) {}

  // SeqScan 的 RowSource 适配器：把物理表游标接入统一的火山模型接口。
  Result<std::optional<RowEntry>> Next() override { return cursor_.Next(); }

 private:
  HeapTable::ScanCursor cursor_;
};

class FilterSource final : public RowSource {
 public:
  FilterSource(std::unique_ptr<RowSource> child, std::size_t column_index, Predicate predicate,
               Schema schema)
      : child_(std::move(child)),
        column_index_(column_index),
        predicate_(std::move(predicate)),
        schema_(std::move(schema)) {}

  Result<std::optional<RowEntry>> Next() override {
    // 惰性过滤：每次只向下游索要一行，匹配才返回；因此不会先把整表装入内存。
    while (true) {
      auto input = child_->Next();
      if (!input.ok()) return Result<std::optional<RowEntry>>(input.status());
      if (!input.value().has_value()) return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
      auto entry = std::move(input.value().value());
      if (entry.second.size() != schema_.size()) {
        return Result<std::optional<RowEntry>>(
            Status::InvalidArgument("Filter 输入行列数与 Schema 不符"));
      }
      const auto &value = entry.second[column_index_];
      if (value.type() != predicate_.value.type()) {
        return Result<std::optional<RowEntry>>(
            Status::TypeMismatch("Filter 列和值类型不匹配: " + predicate_.column));
      }
      if (value == predicate_.value) {
        return Result<std::optional<RowEntry>>(
            std::optional<RowEntry>(std::move(entry)));
      }
    }
  }

 private:
  std::unique_ptr<RowSource> child_;
  std::size_t column_index_{0};
  Predicate predicate_;
  Schema schema_;
};

class ProjectSource final : public RowSource {
 public:
  ProjectSource(std::unique_ptr<RowSource> child, std::vector<std::size_t> indexes,
                Schema schema)
      : child_(std::move(child)), indexes_(std::move(indexes)), schema_(std::move(schema)) {}

  Result<std::optional<RowEntry>> Next() override {
    // 投影只重排/截取 Row 中的列，RID 保留给 DELETE 或未来 UPDATE 定位原记录。
    auto input = child_->Next();
    if (!input.ok()) return Result<std::optional<RowEntry>>(input.status());
    if (!input.value().has_value()) return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
    auto entry = std::move(input.value().value());
    if (entry.second.size() != schema_.size()) {
      return Result<std::optional<RowEntry>>(
          Status::InvalidArgument("Project 输入行列数与 Schema 不符"));
    }
    Row projected;
    projected.reserve(indexes_.size());
    for (const auto index : indexes_) projected.push_back(entry.second[index]);
    entry.second = std::move(projected);
    return Result<std::optional<RowEntry>>(std::optional<RowEntry>(std::move(entry)));
  }

 private:
  std::unique_ptr<RowSource> child_;
  std::vector<std::size_t> indexes_;
  Schema schema_;
};

class MaterializedSource final : public RowSource {
 public:
  explicit MaterializedSource(std::vector<RowEntry> rows) noexcept
      : rows_(std::move(rows)) {}

  Result<std::optional<RowEntry>> Next() override {
    if (next_ == rows_.size()) {
      return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
    }
    return Result<std::optional<RowEntry>>(
        std::optional<RowEntry>(std::move(rows_[next_++])));
  }

 private:
  std::vector<RowEntry> rows_;
  std::size_t next_{0};
};

Result<std::vector<RowEntry>> Materialize(std::unique_ptr<RowSource> child,
                                         const char *executor_name) {
  if (child == nullptr) {
    return Result<std::vector<RowEntry>>(
        Status::InvalidArgument(std::string(executor_name) + " 缺少子算子"));
  }
  std::vector<RowEntry> rows;
  while (true) {
    auto entry = child->Next();
    if (!entry.ok()) return Result<std::vector<RowEntry>>(entry.status());
    if (!entry.value().has_value()) break;
    rows.push_back(std::move(entry.value().value()));
  }
  return Result<std::vector<RowEntry>>(std::move(rows));
}

int CompareValue(const Value &left, const Value &right) {
  if (left.IsInt()) {
    return left.AsInt() < right.AsInt() ? -1
           : left.AsInt() > right.AsInt() ? 1
                                          : 0;
  }
  return left.AsVarchar() < right.AsVarchar() ? -1
         : left.AsVarchar() > right.AsVarchar() ? 1
                                                : 0;
}

bool SameGroup(const Row &left, const Row &right,
               const std::vector<std::size_t> &indexes) {
  for (const auto index : indexes) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

Result<Value> EvaluateUpdateExpression(const CompileExprPtr &expression,
                                       const Row &row,
                                       const Schema &schema) {
  if (expression == nullptr) {
    return Result<Value>(Status::InvalidArgument("UPDATE SET 表达式为空"));
  }
  return std::visit(
      [&](const auto &node) -> Result<Value> {
        using Type = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          auto index = schema.FindColumnIndex(node.name);
          if (!index.ok()) return Result<Value>(index.status());
          return Result<Value>(row[index.value()]);
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          if (node.kind == CompileLiteralKind::Int) {
            return Result<Value>(Value(node.integer));
          }
          if (node.kind == CompileLiteralKind::String) {
            return Result<Value>(Value(node.text));
          }
          return Result<Value>(
              Status::TypeMismatch("UPDATE SET 不能产生 BOOL 值"));
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          auto operand = EvaluateUpdateExpression(node.operand, row, schema);
          if (!operand.ok()) return operand;
          if (node.op != "-" || !operand.value().IsInt()) {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 不支持的一元运算符: " + node.op));
          }
          if (operand.value().AsInt() == std::numeric_limits<std::int64_t>::min()) {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 整数运算溢出"));
          }
          return Result<Value>(Value(-operand.value().AsInt()));
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          auto left = EvaluateUpdateExpression(node.left, row, schema);
          if (!left.ok()) return left;
          auto right = EvaluateUpdateExpression(node.right, row, schema);
          if (!right.ok()) return right;
          if (!left.value().IsInt() || !right.value().IsInt()) {
            return Result<Value>(Status::TypeMismatch(
                "UPDATE SET 算术运算需要 INT 操作数"));
          }
          const auto lhs = left.value().AsInt();
          const auto rhs = right.value().AsInt();
          std::int64_t value = 0;
          bool valid = false;
          if (node.op == "+") {
            valid = !((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
                      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs));
            if (valid) value = lhs + rhs;
          } else if (node.op == "-") {
            valid = !((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) ||
                      (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs));
            if (valid) value = lhs - rhs;
          } else if (node.op == "*") {
            if (lhs == 0 || rhs == 0) {
              valid = true;
              value = 0;
            } else if (!((lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) ||
                         (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()))) {
              if (lhs > 0 && rhs > 0) valid = lhs <= std::numeric_limits<std::int64_t>::max() / rhs;
              if (lhs > 0 && rhs < 0) valid = rhs >= std::numeric_limits<std::int64_t>::min() / lhs;
              if (lhs < 0 && rhs > 0) valid = lhs >= std::numeric_limits<std::int64_t>::min() / rhs;
              if (lhs < 0 && rhs < 0) valid = lhs >= std::numeric_limits<std::int64_t>::max() / rhs;
              if (valid) value = lhs * rhs;
            }
          } else if (node.op == "/") {
            valid = rhs != 0 &&
                    !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1);
            if (valid) value = lhs / rhs;
          } else {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 不支持的二元运算符: " + node.op));
          }
          if (!valid) {
            return Result<Value>(Status::InvalidArgument(
                node.op == "/" && rhs == 0 ? "UPDATE SET 除数不能为零"
                                           : "UPDATE SET 整数运算溢出"));
          }
          return Result<Value>(Value(value));
        } else {
          // BETWEEN、IN 和聚合属于 SELECT/HAVING 编译层谓词，UPDATE SET
          // 尚未提供这些值语义，必须显式拒绝。
          return Result<Value>(Status::NotImplemented(
              "UPDATE SET 暂不支持 BETWEEN、IN 或聚合表达式"));
        }
      },
      expression->data);
}

Result<const TableMetadata *> RequireMetadata(Catalog *catalog, const std::string &table_name) {
  if (catalog == nullptr) {
    return Result<const TableMetadata *>(Status::InvalidArgument("执行器缺少 Catalog"));
  }
  auto metadata = catalog->GetTableMetadata(table_name);
  if (!metadata.ok()) return Result<const TableMetadata *>(metadata.status());
  if (metadata.value() == nullptr) {
    return Result<const TableMetadata *>(Status::InternalError("Catalog 返回空表元数据: " + table_name));
  }
  return metadata;
}

}  // namespace

Result<ExecutionResult> CreateTableExecutor::Execute(const CreateTablePlan &plan) const {
  if (catalog_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("CreateTableExecutor 缺少 Catalog"));
  }
  // 首数据页由持久化 Catalog 分配，计划层只提供逻辑表名和 Schema。
  auto status = catalog_->CreateTable(TableMetadata{plan.table_name, plan.schema, INVALID_PAGE_ID});
  if (!status.ok()) return Result<ExecutionResult>(Contextualize("CreateTableExecutor", status));
  return Result<ExecutionResult>(ExecutionResult{});
}

Result<ExecutionResult> InsertExecutor::Execute(const InsertPlan &plan) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("InsertExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) return Result<ExecutionResult>(Contextualize("InsertExecutor", metadata.status()));
  // Catalog 给出 schema 与首数据页；HeapTable 用它们定位物理表。
  // 数据库执行层当前只保留原有“单行、按 Schema 全列顺序”的写入路径。
  // 指定列和多行 INSERT 已完成 AST、语义检查和 InsertPlan 表达，但在没有
  // 默认值/批量写入语义前必须显式拒绝，不能把部分列或后续行静默丢弃。
  if (!plan.columns.empty() || plan.rows.size() != 1) {
    return Result<ExecutionResult>(Status::NotImplemented(
        "指定列或多行 INSERT 仅编译层支持，未接入数据库执行"));
  }

  HeapTable table(buffer_pool_, *metadata.value());
  auto rid = table.InsertRow(plan.rows.front());
  if (!rid.ok()) return Result<ExecutionResult>(Contextualize("InsertExecutor", rid.status()));
  IndexManager index_manager(catalog_, buffer_pool_);
  auto index_status =
      index_manager.OnInsert(plan.table_name, plan.rows.front(), rid.value());
  if (!index_status.ok()) {
    (void)table.DeleteRow(rid.value());
    return Result<ExecutionResult>(
        Contextualize("InsertExecutor", index_status));
  }
  ExecutionResult result;
  result.affected_rows = 1;
  result.rids.push_back(rid.value());
  return Result<ExecutionResult>(std::move(result));
}

Result<std::unique_ptr<RowSource>> SeqScanExecutor::Execute(const SeqScanPlan &plan) const {
  if (buffer_pool_ == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("SeqScanExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) {
    return Result<std::unique_ptr<RowSource>>(Contextualize("SeqScanExecutor", metadata.status()));
  }
  HeapTable table(buffer_pool_, *metadata.value());
  auto cursor = table.BeginScan();
  if (!cursor.ok()) {
    return Result<std::unique_ptr<RowSource>>(Contextualize("SeqScanExecutor", cursor.status()));
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<HeapTableSource>(std::move(cursor.value()));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> FilterExecutor::Execute(
    const FilterPlan &plan, std::unique_ptr<RowSource> child, const Schema &schema) const {
  if (child == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("FilterExecutor 缺少子算子"));
  }
  auto column = schema.FindColumnIndex(plan.predicate.column);
  if (!column.ok()) return Result<std::unique_ptr<RowSource>>(column.status());
  if (schema.At(column.value()).type != plan.predicate.value.type()) {
    return Result<std::unique_ptr<RowSource>>(
        Status::TypeMismatch("Filter 列和值类型不匹配: " + plan.predicate.column));
  }
  std::unique_ptr<RowSource> source = std::make_unique<FilterSource>(
      std::move(child), column.value(), plan.predicate, schema);
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> ProjectExecutor::Execute(
    const ProjectPlan &plan, std::unique_ptr<RowSource> child, const Schema &schema) const {
  if (child == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("ProjectExecutor 缺少子算子"));
  }
  std::vector<std::size_t> indexes;
  // 在构建阶段把列名解析成下标，使逐行 Next() 不再重复查找 Schema。
  if (plan.select_all) {
    indexes.reserve(schema.size());
    for (std::size_t index = 0; index < schema.size(); ++index) indexes.push_back(index);
  } else {
    indexes.reserve(plan.columns.size());
    for (const auto &name : plan.columns) {
      auto index = schema.FindColumnIndex(name);
      if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
      indexes.push_back(index.value());
    }
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<ProjectSource>(std::move(child), std::move(indexes), schema);
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> GroupByExecutor::Execute(
    const GroupByPlan &plan, std::unique_ptr<RowSource> child,
    const Schema &schema) const {
  std::vector<std::size_t> indexes;
  indexes.reserve(plan.columns.size());
  for (const auto &name : plan.columns) {
    auto index = schema.FindColumnIndex(name);
    if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
    indexes.push_back(index.value());
  }
  auto input = Materialize(std::move(child), "GroupByExecutor");
  if (!input.ok()) return Result<std::unique_ptr<RowSource>>(input.status());
  std::vector<RowEntry> groups;
  for (auto &entry : input.value()) {
    if (entry.second.size() != schema.size()) {
      return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument(
          "GroupByExecutor 输入行列数与 Schema 不符"));
    }
    const bool exists = std::any_of(
        groups.begin(), groups.end(), [&](const RowEntry &group) {
          return SameGroup(group.second, entry.second, indexes);
        });
    if (!exists) groups.push_back(std::move(entry));
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<MaterializedSource>(std::move(groups));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> OrderByExecutor::Execute(
    const OrderByPlan &plan, std::unique_ptr<RowSource> child,
    const Schema &schema) const {
  std::vector<std::pair<std::size_t, OrderDirection>> keys;
  keys.reserve(plan.keys.size());
  for (const auto &key : plan.keys) {
    auto index = schema.FindColumnIndex(key.column);
    if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
    keys.emplace_back(index.value(), key.direction);
  }
  auto rows = Materialize(std::move(child), "OrderByExecutor");
  if (!rows.ok()) return Result<std::unique_ptr<RowSource>>(rows.status());
  for (const auto &entry : rows.value()) {
    if (entry.second.size() != schema.size()) {
      return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument(
          "OrderByExecutor 输入行列数与 Schema 不符"));
    }
  }
  std::stable_sort(rows.value().begin(), rows.value().end(),
                   [&](const RowEntry &left, const RowEntry &right) {
                     for (const auto &[index, direction] : keys) {
                       const int comparison =
                           CompareValue(left.second[index], right.second[index]);
                       if (comparison == 0) continue;
                       return direction == OrderDirection::Asc
                                  ? comparison < 0
                                  : comparison > 0;
                     }
                     return false;
                   });
  std::unique_ptr<RowSource> source =
      std::make_unique<MaterializedSource>(std::move(rows.value()));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<ExecutionResult> DeleteExecutor::Execute(const DeletePlan &plan) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("DeleteExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", metadata.status()));
  HeapTable table(buffer_pool_, *metadata.value());
  auto cursor = table.BeginScan();
  if (!cursor.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", cursor.status()));

  std::optional<std::size_t> predicate_index;
  if (plan.where.has_value()) {
    auto index = metadata.value()->schema.FindColumnIndex(plan.where->column);
    if (!index.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", index.status()));
    predicate_index = index.value();
    if (metadata.value()->schema.At(index.value()).type != plan.where->value.type()) {
      return Result<ExecutionResult>(Status::TypeMismatch("Delete WHERE 列和值类型不匹配: " +
                                                           plan.where->column));
    }
  }

  ExecutionResult result;
  IndexManager index_manager(catalog_, buffer_pool_);
  // 先扫描取得 RID，再删除。不能只拿 Row 值删除，因为相同内容的行可出现多次。
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", entry.status()));
    if (!entry.value().has_value()) break;
    auto row_entry = std::move(entry.value().value());
    const bool matches = !predicate_index.has_value() ||
                         row_entry.second[predicate_index.value()] == plan.where->value;
    if (!matches) continue;
    auto index_status =
        index_manager.OnDelete(plan.table_name, row_entry.second,
                               row_entry.first);
    if (!index_status.ok()) {
      return Result<ExecutionResult>(
          Contextualize("DeleteExecutor", index_status));
    }
    auto status = table.DeleteRow(row_entry.first);
    if (!status.ok()) {
      (void)index_manager.OnInsert(plan.table_name, row_entry.second,
                                   row_entry.first);
      return Result<ExecutionResult>(Contextualize("DeleteExecutor", status));
    }
    ++result.affected_rows;
  }
  return Result<ExecutionResult>(std::move(result));
}

Result<ExecutionResult> UpdateExecutor::Execute(const UpdatePlan &plan) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(
        Status::InvalidArgument("UpdateExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) {
    return Result<ExecutionResult>(
        Contextualize("UpdateExecutor", metadata.status()));
  }
  const Schema &schema = metadata.value()->schema;
  std::vector<std::size_t> assignment_indexes;
  assignment_indexes.reserve(plan.assignments.size());
  for (const auto &assignment : plan.assignments) {
    auto index = schema.FindColumnIndex(assignment.column);
    if (!index.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", index.status()));
    }
    assignment_indexes.push_back(index.value());
  }
  std::optional<std::size_t> predicate_index;
  if (plan.where.has_value()) {
    auto index = schema.FindColumnIndex(plan.where->column);
    if (!index.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", index.status()));
    }
    predicate_index = index.value();
  }

  HeapTable table(buffer_pool_, *metadata.value());
  auto cursor = table.BeginScan();
  if (!cursor.ok()) {
    return Result<ExecutionResult>(Contextualize("UpdateExecutor", cursor.status()));
  }
  struct Replacement {
    RID old_rid;
    Row old_row;
    Row new_row;
  };
  std::vector<Replacement> replacements;
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", entry.status()));
    }
    if (!entry.value().has_value()) break;
    auto original = std::move(entry.value().value());
    if (predicate_index.has_value() &&
        original.second[*predicate_index] != plan.where->value) {
      continue;
    }
    Row updated = original.second;
    for (std::size_t i = 0; i < plan.assignments.size(); ++i) {
      auto value = EvaluateUpdateExpression(plan.assignments[i].expression,
                                            original.second, schema);
      if (!value.ok()) {
        return Result<ExecutionResult>(
            Contextualize("UpdateExecutor", value.status()));
      }
      const auto &column = schema.At(assignment_indexes[i]);
      if (value.value().type() != column.type) {
        return Result<ExecutionResult>(Status::TypeMismatch(
            "UpdateExecutor: SET 值类型不匹配: " + column.name));
      }
      if (column.type == DataType::Varchar && column.length.has_value() &&
          value.value().AsVarchar().size() > *column.length) {
        return Result<ExecutionResult>(Status::InvalidArgument(
            "UpdateExecutor: SET 字符串超过列长度: " + column.name));
      }
      updated[assignment_indexes[i]] = std::move(value.value());
    }
    replacements.push_back(
        Replacement{original.first, original.second, std::move(updated)});
  }

  ExecutionResult result;
  IndexManager index_manager(catalog_, buffer_pool_);
  for (const auto &replacement : replacements) {
    auto new_rid = table.InsertRow(replacement.new_row);
    if (!new_rid.ok()) {
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", new_rid.status()));
    }
    auto index_status =
        index_manager.OnUpdate(plan.table_name, replacement.old_row,
                               replacement.old_rid, replacement.new_row,
                               new_rid.value());
    if (!index_status.ok()) {
      (void)table.DeleteRow(new_rid.value());
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", index_status));
    }
    auto delete_status = table.DeleteRow(replacement.old_rid);
    if (!delete_status.ok()) {
      (void)index_manager.OnUpdate(plan.table_name, replacement.new_row,
                                   new_rid.value(), replacement.old_row,
                                   replacement.old_rid);
      (void)table.DeleteRow(new_rid.value());
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", delete_status));
    }
    result.rids.push_back(new_rid.value());
    ++result.affected_rows;
  }
  return Result<ExecutionResult>(std::move(result));
}

Result<ExecutionEngine::SourcePlan> ExecutionEngine::BuildSource(const SelectPlan &plan) const {
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) {
    return Result<SourcePlan>(Contextualize("ExecutionEngine", metadata.status()));
  }
  if (plan.root == nullptr) {
    return Result<SourcePlan>(Status::InvalidArgument("ExecutionEngine SELECT 缺少根算子"));
  }

  // 把逻辑计划树转换成可按 Next() 拉取数据的 RowSource 管道。
  // DISTINCT 和 LIMIT 位于 SELECT 根计划修饰位。Parser/Planner 已完成语法和
  // 语义表达，但当前 RowSource 链尚未提供去重与截断算子，必须显式拒绝。
  if (plan.distinct || plan.limit.has_value()) {
    return Result<SourcePlan>(Status::NotImplemented(
        "DISTINCT/LIMIT 仅编译层支持，未接入数据库执行"));
  }

  // 聚合和 HAVING 已完成编译层类型/分组检查，但执行层尚无聚合状态和分组
  // 过滤流程，必须显式拒绝。
  if (plan.has_aggregates || plan.having != nullptr) {
    return Result<SourcePlan>(Status::NotImplemented(
        "聚合函数/HAVING 仅编译层支持，未接入数据库执行"));
  }

  // AS 别名已进入 SelectPlan，但当前单表执行链没有别名解析和输出重命名
  // 逻辑，因此显式拒绝，避免忽略别名后产生看似成功的结果。
  if (!plan.table_alias.empty()) {
    return Result<SourcePlan>(Status::NotImplemented(
        "表别名仅编译层支持，未接入数据库执行"));
  }
  for (const auto &alias : plan.projection_aliases) {
    if (!alias.empty()) {
      return Result<SourcePlan>(Status::NotImplemented(
          "列别名仅编译层支持，未接入数据库执行"));
    }
  }

  std::function<Result<SourcePlan>(const std::shared_ptr<const PlanNode> &)> build_node;
  build_node = [&](const std::shared_ptr<const PlanNode> &node) -> Result<SourcePlan> {
    if (node == nullptr) return Result<SourcePlan>(Status::InvalidArgument("执行计划包含空算子"));
    return std::visit(
        [&](const auto &operation) -> Result<SourcePlan> {
          using Type = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Type, SeqScanPlan>) {
            // 叶子节点产生数据；Filter/Project 节点包装 child，最终组成拉取式流水线。
            SeqScanExecutor executor(catalog_, buffer_pool_);
            auto source = executor.Execute(operation);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            std::vector<std::string> names;
            for (const auto &column : metadata.value()->schema.columns()) names.push_back(column.name);
            return Result<SourcePlan>(SourcePlan{std::move(source.value()), std::move(names)});
          } else if constexpr (std::is_same_v<Type, IndexScanPlan>) {
            if (!operation.key.IsInt()) {
              return Result<SourcePlan>(Status::TypeMismatch(
                  "IndexScan 只支持 INT 等值键"));
            }
            auto index_metadata = catalog_->FindIndex(operation.index_name);
            if (!index_metadata.ok()) {
              return Result<SourcePlan>(index_metadata.status());
            }
            if (index_metadata.value()->table_name != operation.table_name ||
                index_metadata.value()->column_name != operation.column_name) {
              return Result<SourcePlan>(Status::InvalidArgument(
                  "IndexScan 索引元数据与计划不匹配"));
            }
            IndexManager index_manager(catalog_, buffer_pool_);
            auto rids = index_manager.Lookup(operation.index_name,
                                             operation.key.AsInt());
            if (!rids.ok()) return Result<SourcePlan>(rids.status());
            HeapTable table(buffer_pool_, *metadata.value());
            std::vector<RowEntry> rows;
            rows.reserve(rids.value().size());
            for (const auto &rid : rids.value()) {
              auto row = table.GetRow(rid);
              if (!row.ok()) return Result<SourcePlan>(row.status());
              rows.emplace_back(rid, std::move(row.value()));
            }
            std::vector<std::string> names;
            for (const auto &column : metadata.value()->schema.columns()) {
              names.push_back(column.name);
            }
            std::unique_ptr<RowSource> source =
                std::make_unique<MaterializedSource>(std::move(rows));
            return Result<SourcePlan>(
                SourcePlan{std::move(source), std::move(names)});
          } else if constexpr (std::is_same_v<Type, FilterPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            FilterExecutor executor;
            auto source = executor.Execute(operation, std::move(child.value().source),
                                           metadata.value()->schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{std::move(source.value()),
                                                 std::move(child.value().column_names)});
          } else if constexpr (std::is_same_v<Type, GroupByPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            GroupByExecutor executor;
            auto source = executor.Execute(operation,
                                           std::move(child.value().source),
                                           metadata.value()->schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{
                std::move(source.value()),
                std::move(child.value().column_names)});
          } else if constexpr (std::is_same_v<Type, OrderByPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            OrderByExecutor executor;
            auto source = executor.Execute(operation,
                                           std::move(child.value().source),
                                           metadata.value()->schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{
                std::move(source.value()),
                std::move(child.value().column_names)});
          } else {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            ProjectExecutor executor;
            auto source = executor.Execute(operation, std::move(child.value().source),
                                           metadata.value()->schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            std::vector<std::string> names;
            if (operation.select_all) {
              for (const auto &column : metadata.value()->schema.columns()) names.push_back(column.name);
            } else {
              names = operation.columns;
            }
            return Result<SourcePlan>(SourcePlan{std::move(source.value()), std::move(names)});
          }
        },
        node->operation);
  };

  return build_node(plan.root);
}

Result<ExecutionResult> ExecutionEngine::Execute(const Plan &plan) const {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("ExecutionEngine 缺少存储依赖"));
  }
  // DDL/DML 计划的总分派点。新增 UpdatePlan、用户管理 Plan 等都在此接入 Executor。
  return std::visit(
      [&](const auto &operation) -> Result<ExecutionResult> {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, CreateTablePlan>) {
          return CreateTableExecutor(catalog_).Execute(operation);
        } else if constexpr (std::is_same_v<Type, InsertPlan>) {
          return InsertExecutor(catalog_, buffer_pool_).Execute(operation);
        } else if constexpr (std::is_same_v<Type, SelectPlan>) {
          auto source = BuildSource(operation);
          if (!source.ok()) return Result<ExecutionResult>(source.status());
          ExecutionResult result;
          result.column_names = std::move(source.value().column_names);
          // 执行引擎作为管道消费者，持续 Next()，直到 optional 为空表示 EOF。
          while (true) {
            auto entry = source.value().source->Next();
            if (!entry.ok()) return Result<ExecutionResult>(Contextualize("ExecutionEngine", entry.status()));
            if (!entry.value().has_value()) break;
            auto row_entry = std::move(entry.value().value());
            result.rids.push_back(row_entry.first);
            result.rows.push_back(std::move(row_entry.second));
          }
          return Result<ExecutionResult>(std::move(result));
        } else if constexpr (std::is_same_v<Type, DropTablePlan>) {
          return Result<ExecutionResult>(Status::NotImplemented(
              "DROP TABLE 仅编译层支持，未接入数据库执行"));
        } else if constexpr (std::is_same_v<Type, CreateIndexPlan>) {
          auto status = IndexManager(catalog_, buffer_pool_)
                            .CreateIndex(operation.index_name,
                                         operation.table_name,
                                         operation.column_name,
                                         operation.is_unique);
          if (!status.ok()) {
            return Result<ExecutionResult>(
                Contextualize("CreateIndexExecutor", status));
          }
          return Result<ExecutionResult>(ExecutionResult{});
        } else if constexpr (std::is_same_v<Type, DropIndexPlan>) {
          auto status =
              IndexManager(catalog_, buffer_pool_).DropIndex(operation.index_name);
          if (!status.ok()) {
            return Result<ExecutionResult>(
                Contextualize("DropIndexExecutor", status));
          }
          return Result<ExecutionResult>(ExecutionResult{});
        } else if constexpr (std::is_same_v<Type, ExplainPlan>) {
          ExecutionResult result;
          result.column_names = {"plan"};
          result.rows.push_back({Value(ToString(operation.select))});
          return Result<ExecutionResult>(std::move(result));
        } else if constexpr (std::is_same_v<Type, UpdatePlan>) {
          return UpdateExecutor(catalog_, buffer_pool_).Execute(operation);
        } else {
          return DeleteExecutor(catalog_, buffer_pool_).Execute(operation);
        }
      },
      plan);
}

}  // namespace oursql
