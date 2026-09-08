#include "oursql/execution/executor.h"

#include <functional>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

Status Contextualize(const char *layer, const Status &status) {
  const auto message = std::string(layer) + ": " + status.message();
  switch (status.code()) {
    case ErrorCode::InvalidArgument: return Status::InvalidArgument(message);
    case ErrorCode::NotFound: return Status::NotFound(message);
    case ErrorCode::NotImplemented: return Status::NotImplemented(message);
    case ErrorCode::AlreadyExists: return Status::AlreadyExists(message);
    case ErrorCode::TypeMismatch: return Status::TypeMismatch(message);
    case ErrorCode::OutOfSpace: return Status::OutOfSpace(message);
    case ErrorCode::IOError: return Status::IOError(message);
    case ErrorCode::InternalError: return Status::InternalError(message);
    case ErrorCode::Ok: return Status::InternalError(message);
  }
  return Status::InternalError(message);
}

class HeapTableSource final : public RowSource {
 public:
  explicit HeapTableSource(HeapTable::ScanCursor cursor) noexcept : cursor_(std::move(cursor)) {}

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
  HeapTable table(buffer_pool_, *metadata.value());
  auto rid = table.InsertRow(plan.values);
  if (!rid.ok()) return Result<ExecutionResult>(Contextualize("InsertExecutor", rid.status()));
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
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", entry.status()));
    if (!entry.value().has_value()) break;
    auto row_entry = std::move(entry.value().value());
    const bool matches = !predicate_index.has_value() ||
                         row_entry.second[predicate_index.value()] == plan.where->value;
    if (!matches) continue;
    auto status = table.DeleteRow(row_entry.first);
    if (!status.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", status));
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

  std::function<Result<SourcePlan>(const std::shared_ptr<PlanNode> &)> build_node;
  build_node = [&](const std::shared_ptr<PlanNode> &node) -> Result<SourcePlan> {
    if (node == nullptr) return Result<SourcePlan>(Status::InvalidArgument("执行计划包含空算子"));
    return std::visit(
        [&](const auto &operation) -> Result<SourcePlan> {
          using Type = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Type, SeqScanPlan>) {
            SeqScanExecutor executor(catalog_, buffer_pool_);
            auto source = executor.Execute(operation);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            std::vector<std::string> names;
            for (const auto &column : metadata.value()->schema.columns()) names.push_back(column.name);
            return Result<SourcePlan>(SourcePlan{std::move(source.value()), std::move(names)});
          } else if constexpr (std::is_same_v<Type, FilterPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            FilterExecutor executor;
            auto source = executor.Execute(operation, std::move(child.value().source),
                                           metadata.value()->schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{std::move(source.value()),
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
          while (true) {
            auto entry = source.value().source->Next();
            if (!entry.ok()) return Result<ExecutionResult>(Contextualize("ExecutionEngine", entry.status()));
            if (!entry.value().has_value()) break;
            auto row_entry = std::move(entry.value().value());
            result.rids.push_back(row_entry.first);
            result.rows.push_back(std::move(row_entry.second));
          }
          return Result<ExecutionResult>(std::move(result));
        } else {
          return DeleteExecutor(catalog_, buffer_pool_).Execute(operation);
        }
      },
      plan);
}

Status Executor::Execute(const Plan &, Catalog &) const {
  return Status::NotImplemented("Executor 兼容接口未绑定 BufferPoolManager，请使用 ExecutionEngine");
}

}  // namespace oursql
