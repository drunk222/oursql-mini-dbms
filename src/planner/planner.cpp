#include "oursql/planner/planner.h"

#include <type_traits>
#include <unordered_set>
#include <utility>

namespace oursql {

namespace {

Status CheckPredicate(const Predicate &predicate, const Schema &schema) {
  auto column = schema.FindColumn(predicate.column);
  if (!column.ok()) return Status::NotFound("WHERE 列不存在: " + predicate.column);
  if (column.value()->type != predicate.value.type()) {
    return Status::TypeMismatch("WHERE 列和值类型不匹配: " + predicate.column);
  }
  if (column.value()->type == DataType::Varchar && column.value()->length.has_value() &&
      predicate.value.AsVarchar().size() > *column.value()->length) {
    return Status::InvalidArgument("WHERE 字符串超过列长度: " + predicate.column);
  }
  return Status::Ok();
}

Status CheckSchema(const Schema &schema) {
  if (schema.size() == 0) return Status::InvalidArgument("表结构不能为空");
  std::unordered_set<std::string> names;
  for (const auto &column : schema.columns()) {
    if (column.name.empty()) return Status::InvalidArgument("列名不能为空");
    if (!names.insert(column.name).second) return Status::AlreadyExists("重复列名: " + column.name);
    if (column.type == DataType::Int && column.length.has_value()) {
      return Status::InvalidArgument("INT 列不能设置 VARCHAR 长度: " + column.name);
    }
    if (column.type == DataType::Varchar && column.length.has_value() && *column.length == 0) {
      return Status::InvalidArgument("VARCHAR 长度必须大于 0: " + column.name);
    }
  }
  return Status::Ok();
}

Result<const TableInfo *> RequireTable(const CatalogReader &catalog, const std::string &table_name) {
  auto table = catalog.FindTable(table_name);
  if (!table.ok()) return Result<const TableInfo *>(Status::NotFound("表不存在: " + table_name));
  if (table.value() == nullptr) {
    return Result<const TableInfo *>(Status::InternalError("CatalogReader 返回空表指针: " + table_name));
  }
  return table;
}

Status CheckValues(const std::vector<Value> &values, const Schema &schema) {
  if (values.size() != schema.size()) {
    return Status::InvalidArgument("VALUES 数量与列数量不符: 期望 " + std::to_string(schema.size()) +
                                   "，实际 " + std::to_string(values.size()));
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto &column = schema.At(i);
    if (values[i].type() != column.type) {
      return Status::TypeMismatch("VALUES 第 " + std::to_string(i + 1) + " 项与列 " + column.name +
                                  " 类型不匹配");
    }
    if (column.type == DataType::Varchar && column.length.has_value() &&
        values[i].AsVarchar().size() > *column.length) {
      return Status::InvalidArgument("VALUES 字符串超过列长度: " + column.name);
    }
  }
  return Status::Ok();
}

std::string PredicateText(const Predicate &predicate) {
  return predicate.column + "=" +
         (predicate.value.IsInt() ? predicate.value.ToString() : "'" + predicate.value.AsVarchar() + "'");
}

std::string NodeText(const std::shared_ptr<PlanNode> &node) {
  if (node == nullptr) return "<null>";
  return std::visit(
      [&](const auto &operation) -> std::string {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, SeqScanPlan>) {
          return "SeqScanPlan(table=" + operation.table_name + ")";
        } else if constexpr (std::is_same_v<Type, FilterPlan>) {
          return "FilterPlan(predicate=" + PredicateText(operation.predicate) +
                 ",child=" + NodeText(operation.child) + ")";
        } else {
          std::string columns = operation.select_all ? "*" : "[";
          if (!operation.select_all) {
            for (std::size_t i = 0; i < operation.columns.size(); ++i) {
              if (i != 0) columns += ",";
              columns += operation.columns[i];
            }
            columns += "]";
          }
          return "ProjectPlan(columns=" + columns + ",child=" + NodeText(operation.child) + ")";
        }
      },
      node->operation);
}

}  // namespace

Result<Plan> Planner::Build(const Statement &statement, const CatalogReader &catalog) const {
  return std::visit(
      [&](const auto &value) -> Result<Plan> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CreateTableStatement>) {
          auto schema_status = CheckSchema(value.schema);
          if (!schema_status.ok()) return Result<Plan>(schema_status);
          auto existing = catalog.FindTable(value.table_name);
          if (existing.ok()) return Result<Plan>(Status::AlreadyExists("表已存在: " + value.table_name));
          if (existing.status().code() != ErrorCode::NotFound) return Result<Plan>(existing.status());
          return Result<Plan>(CreateTablePlan{value.table_name, value.schema});
        } else if constexpr (std::is_same_v<Type, InsertStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return Result<Plan>(table.status());
          auto values_status = CheckValues(value.values, table.value()->schema);
          if (!values_status.ok()) return Result<Plan>(values_status);
          return Result<Plan>(InsertPlan{value.table_name, value.values});
        } else if constexpr (std::is_same_v<Type, SelectStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return Result<Plan>(table.status());
          std::unordered_set<std::string> projected;
          if (value.select_all && !value.projection.empty()) {
            return Result<Plan>(Status::InvalidArgument("SELECT * 不能与列名混用"));
          }
          for (const auto &column_name : value.projection) {
            if (!projected.insert(column_name).second) {
              return Result<Plan>(Status::AlreadyExists("SELECT 重复列: " + column_name));
            }
            if (!table.value()->schema.FindColumn(column_name).ok()) {
              return Result<Plan>(Status::NotFound("SELECT 列不存在: " + column_name));
            }
          }
          if (value.where.has_value()) {
            auto predicate_status = CheckPredicate(*value.where, table.value()->schema);
            if (!predicate_status.ok()) return Result<Plan>(predicate_status);
          }
          auto root = std::make_shared<PlanNode>(SeqScanPlan{value.table_name});
          if (value.where.has_value()) root = std::make_shared<PlanNode>(FilterPlan{root, *value.where});
          root = std::make_shared<PlanNode>(ProjectPlan{root, value.projection, value.select_all});
          return Result<Plan>(SelectPlan{value.table_name, std::move(root)});
        } else {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return Result<Plan>(table.status());
          if (value.where.has_value()) {
            auto predicate_status = CheckPredicate(*value.where, table.value()->schema);
            if (!predicate_status.ok()) return Result<Plan>(predicate_status);
          }
          return Result<Plan>(DeletePlan{value.table_name, value.where});
        }
      },
      statement);
}

std::string ToString(const Plan &plan) {
  return std::visit(
      [](const auto &value) -> std::string {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CreateTablePlan>) {
          std::string result = "CreateTablePlan(table=" + value.table_name + ",schema=[";
          for (std::size_t i = 0; i < value.schema.size(); ++i) {
            if (i != 0) result += ",";
            const auto &column = value.schema.At(i);
            result += column.name + ":" + (column.type == DataType::Int ? "INT" : "VARCHAR");
            if (column.length.has_value()) result += "(" + std::to_string(*column.length) + ")";
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, InsertPlan>) {
          std::string result = "InsertPlan(table=" + value.table_name + ",values=[";
          for (std::size_t i = 0; i < value.values.size(); ++i) {
            if (i != 0) result += ",";
            result += value.values[i].IsInt() ? value.values[i].ToString()
                                              : "'" + value.values[i].AsVarchar() + "'";
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, SelectPlan>) {
          return "SelectPlan(table=" + value.table_name + ",root=" + NodeText(value.root) + ")";
        } else {
          std::string result = "DeletePlan(table=" + value.table_name;
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          return result + ")";
        }
      },
      plan);
}

}  // namespace oursql
