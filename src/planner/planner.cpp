#include "oursql/planner/planner.h"

#include "oursql/optimizer/optimizer.h"

#include <functional>
#include <type_traits>
#include <unordered_set>
#include <utility>

// Planner 实现说明：
//
// 1. 静态辅助函数检查 Schema、Predicate、INSERT 值、表达式类型和分组规则；
// 2. Build() 使用 std::visit 穷举每种 Statement；
// 3. SELECT 按扫描叶子 -> Filter -> GroupBy -> OrderBy -> Project 构造；
// 4. 最后读取目标表索引并调用 Optimizer 执行 R1/R2。
//
// Planner 只读 Catalog；所有数据库副作用都留给执行阶段。语义错误保留原
// 错误码，并附加语句位置，便于 CLI 和上层接口统一诊断。
namespace oursql {

namespace {

// 检查已经抽取出的数据库层 Predicate。这里只处理“列 = 字面量”的简单形态，
// 复杂表达式由 CheckCompileExpr() 递归检查。
Status CheckPredicate(const Predicate &predicate, const Schema &schema) {
  auto column = schema.FindColumn(predicate.column);
  if (!column.ok()) return Status::NotFound("WHERE 列不存在: " + predicate.column);
  if (predicate.kind == PredicateKind::IsNull ||
      predicate.kind == PredicateKind::IsNotNull) {
    return Status::Ok();
  }
  if (predicate.value.IsNull()) {
    return Status::InvalidArgument(
        "WHERE 不能使用 = NULL，请改用 IS NULL");
  }
  if (column.value()->type != predicate.value.type()) {
    return Status::TypeMismatch("WHERE 列和值类型不匹配: " + predicate.column);
  }
  if (column.value()->type == DataType::Varchar && column.value()->length.has_value() &&
      predicate.value.AsVarchar().size() > *column.value()->length) {
    return Status::InvalidArgument("WHERE 字符串超过列长度: " + predicate.column);
  }
  return Status::Ok();
}

// CREATE TABLE 的静态 Schema 检查：至少一列、列名唯一、长度属性只用于
// VARCHAR，并拒绝零长度。Catalog 是否已有同名表在 Planner::Build 中检查。
Status CheckSchema(const Schema &schema) {
  if (schema.size() == 0) return Status::InvalidArgument("表结构不能为空");
  std::unordered_set<std::string> names;
  std::size_t primary_key_count = 0;
  for (const auto &column : schema.columns()) {
    if (column.name.empty()) return Status::InvalidArgument("列名不能为空");
    if (!names.insert(column.name).second) return Status::AlreadyExists("重复列名: " + column.name);
    if (column.type == DataType::Int && column.length.has_value()) {
      return Status::InvalidArgument("INT 列不能设置 VARCHAR 长度: " + column.name);
    }
    if (column.type == DataType::Varchar && column.length.has_value() && *column.length == 0) {
      return Status::InvalidArgument("VARCHAR 长度必须大于 0: " + column.name);
    }
    if (column.primary_key) {
      ++primary_key_count;
      if (column.nullable) {
        return Status::InvalidArgument(
            "PRIMARY KEY 列不能允许 NULL: " + column.name);
      }
    }
    if (column.default_value.has_value()) {
      const auto &default_value = *column.default_value;
      if (default_value.IsNull()) {
        if (!column.nullable) {
          return Status::InvalidArgument(
              "NOT NULL 列的 DEFAULT 不能是 NULL: " + column.name);
        }
      } else {
        if (default_value.type() != column.type) {
          return Status::TypeMismatch(
              "DEFAULT 类型与列不匹配: " + column.name);
        }
        if (column.type == DataType::Varchar &&
            column.length.has_value() &&
            default_value.AsVarchar().size() > *column.length) {
          return Status::InvalidArgument(
              "DEFAULT 字符串超过列长度: " + column.name);
        }
      }
    }
  }
  if (primary_key_count > 1) {
    return Status::InvalidArgument("当前版本每张表最多支持一个 PRIMARY KEY 列");
  }
  return Status::Ok();
}

// 将找不到表、Catalog 返回空指针等不同失败统一成带表名的语义错误。
Result<const TableInfo *> RequireTable(const CatalogReader &catalog, const std::string &table_name) {
  auto table = catalog.FindTable(table_name);
  if (!table.ok()) return Result<const TableInfo *>(Status::NotFound("表不存在: " + table_name));
  if (table.value() == nullptr) {
    return Result<const TableInfo *>(Status::InternalError("CatalogReader 返回空表指针: " + table_name));
  }
  return table;
}

// Planner 的错误发生在语句级别，因此这里把语句起始位置追加到 Status，
// 不改变原始错误码，调用者仍可按 NotFound/TypeMismatch 等类型处理。
std::string PositionText(const Position &position) {
  return "，位置 " + std::to_string(position.line) + ":" +
         std::to_string(position.column);
}

// 保留原错误码，只把语句位置附加到诊断文本。
Status WithLocation(Status status, const Position &position) {
  if (status.ok()) return status;
  const std::string message = status.message() + PositionText(position);
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

// 检查单个 INSERT 值是否满足目标列的类型和 VARCHAR 长度约束。
Status CheckInsertValue(const Value &value, const Column &column,
                        const std::string &context) {
  if (value.IsNull()) {
    if (!column.nullable) {
      return Status::InvalidArgument(context + "列不允许 NULL");
    }
    return Status::Ok();
  }
  if (value.type() != column.type) {
    return Status::TypeMismatch(context + "类型不匹配");
  }
  if (column.type == DataType::Varchar && column.length.has_value() &&
      value.AsVarchar().size() > *column.length) {
    return Status::InvalidArgument(context + "字符串超过列长度");
  }
  return Status::Ok();
}

// 检查并规范化 INSERT 的全部 VALUES 行：
// - columns 为空时，每行按 Schema 定义顺序对应所有列；
// - columns 非空时，目标列必须存在且不能重复；
// - 未提供列依次使用 DEFAULT、NULL（列允许时），否则报告缺少 NOT NULL；
// - 返回的每行都严格按 Schema 顺序排列，执行器不再猜测省略列语义。
Result<std::vector<std::vector<Value>>> NormalizeInsertRows(
    const std::vector<std::vector<Value>> &rows,
    const std::vector<std::string> &columns, const Schema &schema) {
  if (rows.empty()) {
    return Result<std::vector<std::vector<Value>>>(
        Status::InvalidArgument("INSERT 至少需要一行 VALUES"));
  }

  std::vector<std::size_t> target_indexes;
  if (columns.empty()) {
    target_indexes.reserve(schema.size());
    for (std::size_t i = 0; i < schema.size(); ++i) {
      target_indexes.push_back(i);
    }
  } else {
    std::unordered_set<std::string> seen_columns;
    target_indexes.reserve(columns.size());
    for (const auto &column_name : columns) {
      if (!seen_columns.insert(column_name).second) {
        return Result<std::vector<std::vector<Value>>>(
            Status::AlreadyExists("INSERT 目标列重复: " + column_name));
      }
      auto column = schema.FindColumnIndex(column_name);
      if (!column.ok()) {
        return Result<std::vector<std::vector<Value>>>(
            Status::NotFound("INSERT 目标列不存在: " + column_name));
      }
      target_indexes.push_back(column.value());
    }
  }

  std::vector<std::vector<Value>> normalized_rows;
  normalized_rows.reserve(rows.size());
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    const auto &row = rows[row_index];
    if (row.size() != target_indexes.size()) {
      return Result<std::vector<std::vector<Value>>>(Status::InvalidArgument(
          "INSERT 第 " + std::to_string(row_index + 1) +
          " 行 VALUES 数量与目标列数量不符: 期望 " +
          std::to_string(target_indexes.size()) + "，实际 " +
          std::to_string(row.size())));
    }
    std::vector<Value> normalized(schema.size());
    std::vector<bool> provided(schema.size(), false);
    for (std::size_t value_index = 0; value_index < row.size();
         ++value_index) {
      const std::size_t column_index = target_indexes[value_index];
      const auto &column = schema.At(column_index);
      const std::string context =
          "INSERT 第 " + std::to_string(row_index + 1) + " 行第 " +
          std::to_string(value_index + 1) + " 项与列 " + column.name + " ";
      auto status = CheckInsertValue(row[value_index], column, context);
      if (!status.ok()) {
        return Result<std::vector<std::vector<Value>>>(status);
      }
      normalized[column_index] = row[value_index];
      provided[column_index] = true;
    }
    for (std::size_t column_index = 0; column_index < schema.size();
         ++column_index) {
      if (provided[column_index]) continue;
      const auto &column = schema.At(column_index);
      if (column.default_value.has_value()) {
        normalized[column_index] = *column.default_value;
      } else if (column.nullable) {
        normalized[column_index] = Value();
      } else {
        return Result<std::vector<std::vector<Value>>>(Status::InvalidArgument(
            "INSERT 缺少 NOT NULL 列: " + column.name));
      }
    }
    normalized_rows.push_back(std::move(normalized));
  }
  return Result<std::vector<std::vector<Value>>>(
      std::move(normalized_rows));
}

// 以下文本打印函数只用于测试和调试，不参与执行或再次解析。
std::string PredicateText(const Predicate &predicate) {
  if (predicate.kind == PredicateKind::IsNull) {
    return predicate.column + " is null";
  }
  if (predicate.kind == PredicateKind::IsNotNull) {
    return predicate.column + " is not null";
  }
  if (predicate.value.IsNull()) {
    return predicate.column + "=NULL";
  }
  return predicate.column + "=" +
         (predicate.value.IsInt() ? predicate.value.ToString() : "'" + predicate.value.AsVarchar() + "'");
}

// 将 CompileExpr 打印为稳定的带括号形式；文本仅用于诊断。
std::string CompileExprText(const CompileExprPtr &expr) {
  if (expr == nullptr) return "<null>";
  return std::visit(
      [&](const auto &value) -> std::string {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          return value.name;
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          if (value.kind == CompileLiteralKind::Int) {
            return std::to_string(value.integer);
          }
          if (value.kind == CompileLiteralKind::String) {
            return "'" + value.text + "'";
          }
          if (value.kind == CompileLiteralKind::Null) return "NULL";
          return value.text;
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          return "(" + CompileExprText(value.left) + " " + value.op + " " +
                 CompileExprText(value.right) + ")";
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          return "(" + value.op + " " + CompileExprText(value.operand) + ")";
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          return "(" + CompileExprText(value.operand) +
                 (value.negated ? " is not null)" : " is null)");
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          return "(" + CompileExprText(value.operand) + " " +
                 (value.negated ? "not between " : "between ") +
                 CompileExprText(value.lower) + " and " +
                 CompileExprText(value.upper) + ")";
        } else if constexpr (std::is_same_v<Type, CompileInExpr>) {
          std::string result = "(" + CompileExprText(value.operand) + " " +
                               (value.negated ? "not in (" : "in (");
          for (std::size_t i = 0; i < value.options.size(); ++i) {
            if (i != 0) result += ", ";
            result += CompileExprText(value.options[i]);
          }
          return result + "))";
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          if (value.kind == CompileSubqueryKind::Exists) {
            return "(exists (select ...))";
          }
          if (value.kind == CompileSubqueryKind::In) {
            return "(" + CompileExprText(value.operand) +
                   (value.negated ? " not in (select ...))"
                                  : " in (select ...))");
          }
          return "(select ...)";
        } else {
          std::string result;
          switch (value.kind) {
            case CompileAggregateKind::Count: result = "count("; break;
            case CompileAggregateKind::Sum: result = "sum("; break;
            case CompileAggregateKind::Avg: result = "avg("; break;
            case CompileAggregateKind::Max: result = "max("; break;
            case CompileAggregateKind::Min: result = "min("; break;
          }
          result += value.star ? "*" : CompileExprText(value.argument);
          return result + ")";
        }
      },
      expr->data);
}

// 计划树文本采用父算子嵌套 child 的格式，保持稳定以便比较优化前后结果。
std::string NodeText(const std::shared_ptr<const PlanNode> &node) {
  if (node == nullptr) return "<null>";
  return std::visit(
      [&](const auto &operation) -> std::string {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, SeqScanPlan>) {
          return "SeqScanPlan(table=" + operation.table_name + ")";
        } else if constexpr (std::is_same_v<Type, IndexScanPlan>) {
          return "IndexScanPlan(table=" + operation.table_name +
                 ",index=" + operation.index_name +
                 ",column=" + operation.column_name +
                 ",key=" + operation.key.ToString() + ")";
        } else if constexpr (std::is_same_v<Type, FilterPlan>) {
          return "FilterPlan(predicate=" + PredicateText(operation.predicate) +
                 ",child=" + NodeText(operation.child) + ")";
        } else if constexpr (std::is_same_v<Type, JoinPlan>) {
          std::string result =
              "JoinPlan(type=" +
              std::string(operation.join_type == JoinType::Inner
                              ? "inner"
                              : operation.join_type == JoinType::Left
                                    ? "left"
                                    : operation.join_type == JoinType::Right
                                          ? "right"
                                          : "full") +
              ",left_table=" + operation.left_table +
              ",left_column=" + operation.left_column;
          if (operation.left_column_index.has_value()) {
            result += ",left_index=" +
                      std::to_string(*operation.left_column_index);
          }
          result += ",right_table=" + operation.right_table +
                    ",right_column=" + operation.right_column;
          if (operation.right_column_index.has_value()) {
            result += ",right_index=" +
                      std::to_string(*operation.right_column_index);
          }
          return result + ",left=" + NodeText(operation.left) +
                 ",right=" + NodeText(operation.right) + ")";
        } else if constexpr (std::is_same_v<Type, GroupByPlan>) {
          std::string columns = "[";
          for (std::size_t i = 0; i < operation.columns.size(); ++i) {
            if (i != 0) columns += ",";
            columns += operation.columns[i];
          }
          columns += "]";
          return "GroupByPlan(columns=" + columns +
                 ",child=" + NodeText(operation.child) + ")";
        } else if constexpr (std::is_same_v<Type, OrderByPlan>) {
          std::string keys = "[";
          for (std::size_t i = 0; i < operation.keys.size(); ++i) {
            if (i != 0) keys += ",";
            keys += operation.keys[i].column;
            keys += operation.keys[i].direction == OrderDirection::Desc
                        ? " DESC"
                        : " ASC";
          }
          keys += "]";
          return "OrderByPlan(keys=" + keys +
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
          return "ProjectPlan(columns=" + columns +
                 ",child=" + NodeText(operation.child) + ")";
        }
      },
      node->operation);
}

enum class CompileType { Int, Varchar, Bool, Null, Invalid };

// Invalid 只用于保持 switch 完整性；正常检查会直接返回 Status 错误。
std::string CompileTypeName(CompileType type) {
  switch (type) {
    case CompileType::Int: return "INT";
    case CompileType::Varchar: return "VARCHAR";
    case CompileType::Bool: return "BOOL";
    case CompileType::Null: return "NULL";
    case CompileType::Invalid: return "INVALID";
  }
  return "INVALID";
}

PlanValueType ToPlanValueType(CompileType type) {
  if (type == CompileType::Int) return PlanValueType::Int;
  if (type == CompileType::Varchar) return PlanValueType::Varchar;
  if (type == CompileType::Null) return PlanValueType::Null;
  return PlanValueType::Bool;
}

PlanValueType ToPlanValueType(DataType type) {
  return type == DataType::Int ? PlanValueType::Int : PlanValueType::Varchar;
}

// 递归推导 CompileExpr 的结果类型，同时检查列是否存在和运算符操作数类型。
// context 用于把错误定位到 WHERE、UPDATE SET 等具体语法位置。
Result<CompileType> CheckCompileExpr(const CompileExprPtr &expr,
                                     const Schema &schema,
                                     const std::string &context,
                                     bool allow_aggregates = false,
                                     const CatalogView *catalog = nullptr) {
  if (expr == nullptr) {
    return Result<CompileType>(
        Status::InvalidArgument(context + ": 表达式为空"));
  }
  return std::visit(
      [&](const auto &value) -> Result<CompileType> {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          // 列引用必须存在于当前表 Schema；Catalog 只读，不在编译阶段登记表。
          auto column = schema.FindColumn(value.name);
          if (!column.ok()) {
            return Result<CompileType>(
                Status::NotFound(context + ": 列不存在: " + value.name));
          }
          return Result<CompileType>(
              column.value()->type == DataType::Int ? CompileType::Int
                                                    : CompileType::Varchar);
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          // 编译器内部区分 INT、VARCHAR、BOOL 和 NULL 四种字面量类型。
          if (value.kind == CompileLiteralKind::Int) {
            return Result<CompileType>(CompileType::Int);
          }
          if (value.kind == CompileLiteralKind::String) {
            return Result<CompileType>(CompileType::Varchar);
          }
          if (value.kind == CompileLiteralKind::Null) {
            return Result<CompileType>(CompileType::Null);
          }
          return Result<CompileType>(CompileType::Bool);
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          // NOT 要求 BOOL 操作数，一元负号只允许 INT。
          auto operand = CheckCompileExpr(
              value.operand, schema, context, allow_aggregates, catalog);
          if (!operand.ok()) return operand;
          if (operand.value() == CompileType::Null) {
            return Result<CompileType>(CompileType::Null);
          }
          if (value.op == "not") {
            if (operand.value() != CompileType::Bool) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": NOT 需要 BOOL，实际 " +
                  CompileTypeName(operand.value())));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          if (value.op == "-") {
            if (operand.value() != CompileType::Int) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": 一元负号需要 INT，实际 " +
                  CompileTypeName(operand.value())));
            }
            return Result<CompileType>(CompileType::Int);
          }
          return Result<CompileType>(
              Status::InvalidArgument(context + ": 未知一元运算符 " + value.op));
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          auto operand = CheckCompileExpr(
              value.operand, schema, context, allow_aggregates, catalog);
          if (!operand.ok()) return operand;
          return Result<CompileType>(CompileType::Bool);
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          // BETWEEN 要求操作数、下界和上界可比较，当前实现要求三者类型一致。
          auto operand = CheckCompileExpr(
              value.operand, schema, context, allow_aggregates, catalog);
          if (!operand.ok()) return operand;
          auto lower = CheckCompileExpr(value.lower, schema, context,
                                        allow_aggregates, catalog);
          if (!lower.ok()) return lower;
          auto upper = CheckCompileExpr(value.upper, schema, context,
                                        allow_aggregates, catalog);
          if (!upper.ok()) return upper;
          if (operand.value() != CompileType::Null &&
              lower.value() != CompileType::Null &&
              upper.value() != CompileType::Null &&
              (operand.value() != lower.value() ||
               operand.value() != upper.value())) {
            return Result<CompileType>(Status::TypeMismatch(
                context + ": BETWEEN 操作数类型不一致"));
          }
          return Result<CompileType>(CompileType::Bool);
        } else if constexpr (std::is_same_v<Type, CompileInExpr>) {
          // IN 列表至少一项；操作数与所有候选值的类型必须一致。
          if (value.options.empty()) {
            return Result<CompileType>(
                Status::InvalidArgument(context + ": IN 列表不能为空"));
          }
          auto operand = CheckCompileExpr(
              value.operand, schema, context, allow_aggregates, catalog);
          if (!operand.ok()) return operand;
          for (const auto &option : value.options) {
            auto option_type = CheckCompileExpr(
                option, schema, context, allow_aggregates, catalog);
            if (!option_type.ok()) return option_type;
            if (option_type.value() != CompileType::Null &&
                operand.value() != CompileType::Null &&
                option_type.value() != operand.value()) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": IN 操作数和候选值类型不一致"));
            }
          }
          return Result<CompileType>(CompileType::Bool);
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          if (value.query == nullptr) {
            return Result<CompileType>(Status::InvalidArgument(
                context + ": 子查询缺少 SELECT 本体"));
          }
          if (catalog == nullptr) {
            return Result<CompileType>(Status::InternalError(
                context + ": 子查询语义检查缺少只读 Catalog"));
          }
          auto subquery_plan = Planner().Build(*value.query, *catalog);
          if (!subquery_plan.ok()) {
            return Result<CompileType>(subquery_plan.status());
          }
          const auto *subquery_select =
              std::get_if<SelectPlan>(&subquery_plan.value());
          if (subquery_select == nullptr) {
            return Result<CompileType>(Status::InternalError(
                context + ": 子查询没有生成 SelectPlan"));
          }
          if (value.kind == CompileSubqueryKind::Exists) {
            return Result<CompileType>(CompileType::Bool);
          }
          if (subquery_select->output_types.size() != 1) {
            return Result<CompileType>(Status::InvalidArgument(
                context + ": 子查询必须返回恰好一列，实际 " +
                std::to_string(subquery_select->output_types.size())));
          }
          if (subquery_select->output_types.front() == PlanValueType::Bool) {
            return Result<CompileType>(Status::TypeMismatch(
                context + ": 标量/IN 子查询不能返回 BOOL"));
          }
          const PlanValueType subquery_type =
              subquery_select->output_types.front();
          const CompileType result_type =
              subquery_type == PlanValueType::Int
                  ? CompileType::Int
                  : subquery_type == PlanValueType::Null
                        ? CompileType::Null
                        : CompileType::Varchar;
          if (value.kind == CompileSubqueryKind::In) {
            auto operand = CheckCompileExpr(
                value.operand, schema, context, allow_aggregates, catalog);
            if (!operand.ok()) return operand;
            if (operand.value() != CompileType::Null &&
                result_type != CompileType::Null &&
                operand.value() != result_type) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": IN 子查询两侧类型不一致"));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          return Result<CompileType>(result_type);
        } else if constexpr (std::is_same_v<Type, CompileAggregateExpr>) {
          if (!allow_aggregates) {
            return Result<CompileType>(Status::InvalidArgument(
                context + ": 聚合函数只能用于 SELECT 列表或 HAVING"));
          }
          if (value.star) {
            if (value.kind != CompileAggregateKind::Count) {
              return Result<CompileType>(Status::InvalidArgument(
                  context + ": 只有 COUNT 支持 '*'"));
            }
            return Result<CompileType>(CompileType::Int);
          }
          if (value.argument == nullptr) {
            return Result<CompileType>(
                Status::InvalidArgument(context + ": 聚合函数缺少参数"));
          }
          // 聚合参数内部禁止再次出现聚合，避免 COUNT(SUM(x)) 这类嵌套。
          auto argument = CheckCompileExpr(value.argument, schema, context,
                                           false, catalog);
          if (!argument.ok()) return argument;
          if (value.kind == CompileAggregateKind::Count) {
            return Result<CompileType>(CompileType::Int);
          }
          if (value.kind == CompileAggregateKind::Sum ||
              value.kind == CompileAggregateKind::Avg) {
            if (argument.value() != CompileType::Int) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": SUM/AVG 只支持 INT"));
            }
            return Result<CompileType>(CompileType::Int);
          }
          if (argument.value() == CompileType::Bool) {
            return Result<CompileType>(Status::TypeMismatch(
                context + ": MIN/MAX 不支持 BOOL"));
          }
          return argument;
        } else {
          // 二元表达式先递归检查左右子树，再组合并检查结果类型。
          auto left = CheckCompileExpr(value.left, schema, context,
                                       allow_aggregates, catalog);
          if (!left.ok()) return left;
          auto right = CheckCompileExpr(value.right, schema, context,
                                        allow_aggregates, catalog);
          if (!right.ok()) return right;
          if (value.op == "and" || value.op == "or") {
            if (left.value() == CompileType::Null ||
                right.value() == CompileType::Null) {
              return Result<CompileType>(CompileType::Null);
            }
            if (left.value() != CompileType::Bool ||
                right.value() != CompileType::Bool) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": " + value.op + " 需要 BOOL 操作数"));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          if (value.op == "+" || value.op == "-" || value.op == "*" ||
              value.op == "/") {
            if (left.value() == CompileType::Null ||
                right.value() == CompileType::Null) {
              return Result<CompileType>(CompileType::Null);
            }
            if (left.value() != CompileType::Int ||
                right.value() != CompileType::Int) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": 算术运算需要 INT 操作数"));
            }
            return Result<CompileType>(CompileType::Int);
          }
          if (value.op == "like" || value.op == "not like") {
            if (left.value() == CompileType::Null ||
                right.value() == CompileType::Null) {
              return Result<CompileType>(CompileType::Null);
            }
            if (left.value() != CompileType::Varchar ||
                right.value() != CompileType::Varchar) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": LIKE 两侧必须是 VARCHAR"));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          if (value.op == ">" || value.op == ">=" || value.op == "<" ||
              value.op == "<=" || value.op == "=" || value.op == "!=" ||
              value.op == "<>") {
            if (value.op == "=" &&
                (left.value() == CompileType::Null ||
                 right.value() == CompileType::Null)) {
              return Result<CompileType>(Status::InvalidArgument(
                  context + ": 不能使用 = NULL，请改用 IS NULL"));
            }
            if (left.value() == CompileType::Null ||
                right.value() == CompileType::Null) {
              return Result<CompileType>(CompileType::Bool);
            }
            if (left.value() != right.value()) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": 比较运算两侧类型不一致"));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          return Result<CompileType>(
              Status::InvalidArgument(context + ": 未知二元运算符 " + value.op));
        }
      },
      expr->data);
}

Status CheckSingleTableQualifiers(const CompileExprPtr &expr,
                                  std::string_view table_name,
                                  std::string_view table_alias,
                                  const std::string &context) {
  if (expr == nullptr) return Status::Ok();
  return std::visit(
      [&](const auto &node) -> Status {
        using Type = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          if (!node.table_name.empty() && node.table_name != table_name &&
              node.table_name != table_alias) {
            return Status::NotFound(context + ": 表名或别名不匹配: " +
                                    node.table_name);
          }
          return Status::Ok();
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          return Status::Ok();
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          return CheckSingleTableQualifiers(node.operand, table_name,
                                            table_alias, context);
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr> ||
                             std::is_same_v<Type, CompileIsNullExpr>) {
          return CheckSingleTableQualifiers(node.operand, table_name,
                                            table_alias, context);
        } else if constexpr (std::is_same_v<Type, CompileAggregateExpr>) {
          return CheckSingleTableQualifiers(node.argument, table_name,
                                            table_alias, context);
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          auto left = CheckSingleTableQualifiers(node.left, table_name,
                                                 table_alias, context);
          return left.ok() ? CheckSingleTableQualifiers(
                                 node.right, table_name, table_alias, context)
                           : left;
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          auto operand = CheckSingleTableQualifiers(node.operand, table_name,
                                                    table_alias, context);
          if (!operand.ok()) return operand;
          auto lower = CheckSingleTableQualifiers(node.lower, table_name,
                                                  table_alias, context);
          return lower.ok() ? CheckSingleTableQualifiers(
                                  node.upper, table_name, table_alias, context)
                            : lower;
        } else {
          auto operand = CheckSingleTableQualifiers(node.operand, table_name,
                                                    table_alias, context);
          if (!operand.ok()) return operand;
          for (const auto &option : node.options) {
            auto status = CheckSingleTableQualifiers(option, table_name,
                                                     table_alias, context);
            if (!status.ok()) return status;
          }
          return Status::Ok();
        }
      }, expr->data);
}

// 递归判断表达式是否包含聚合函数。WHERE 和 UPDATE 不接受聚合；SELECT
// 投影与 HAVING 则使用该结果决定是否需要执行 GROUP BY 分组规则检查。
bool ContainsAggregate(const CompileExprPtr &expr) {
  if (expr == nullptr) return false;
  return std::visit(
      [&](const auto &value) -> bool {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CompileAggregateExpr>) {
          return true;
        } else if constexpr (std::is_same_v<Type, CompileColumnExpr> ||
                             std::is_same_v<Type, CompileLiteralExpr>) {
          return false;
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          return ContainsAggregate(value.operand);
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          return ContainsAggregate(value.operand);
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          return ContainsAggregate(value.left) ||
                 ContainsAggregate(value.right);
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          return ContainsAggregate(value.operand) ||
                 ContainsAggregate(value.lower) ||
                 ContainsAggregate(value.upper);
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          // 子查询拥有独立作用域；子查询内部的聚合不能把外层 SELECT 误判为聚合。
          return false;
        } else {
          if (ContainsAggregate(value.operand)) return true;
          for (const auto &option : value.options) {
            if (ContainsAggregate(option)) return true;
          }
          return false;
        }
      },
      expr->data);
}

// 检查非聚合列是否满足 GROUP BY 约束。聚合节点本身直接放行；其参数已经由
// CheckCompileExpr 单独检查，不能在这里把参数列错误地要求出现在 GROUP BY。
Status CheckGroupedExpr(const CompileExprPtr &expr,
                        const std::unordered_set<std::string> &group_columns,
                        const std::string &context) {
  if (expr == nullptr) return Status::Ok();
  return std::visit(
      [&](const auto &value) -> Status {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          if (group_columns.find(value.name) == group_columns.end()) {
            return Status::InvalidArgument(
                context + ": 非聚合列必须出现在 GROUP BY 中: " + value.name);
          }
          return Status::Ok();
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          return Status::Ok();
        } else if constexpr (std::is_same_v<Type, CompileAggregateExpr>) {
          return Status::Ok();
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          return CheckGroupedExpr(value.operand, group_columns, context);
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          return CheckGroupedExpr(value.operand, group_columns, context);
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          auto left =
              CheckGroupedExpr(value.left, group_columns, context);
          if (!left.ok()) return left;
          return CheckGroupedExpr(value.right, group_columns, context);
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          auto operand =
              CheckGroupedExpr(value.operand, group_columns, context);
          if (!operand.ok()) return operand;
          auto lower =
              CheckGroupedExpr(value.lower, group_columns, context);
          if (!lower.ok()) return lower;
          return CheckGroupedExpr(value.upper, group_columns, context);
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          // 子查询内的列属于嵌套作用域，不是当前 GROUP BY 的候选表达式。
          return Status::Ok();
        } else {
          auto operand =
              CheckGroupedExpr(value.operand, group_columns, context);
          if (!operand.ok()) return operand;
          for (const auto &option : value.options) {
            auto option_status =
                CheckGroupedExpr(option, group_columns, context);
            if (!option_status.ok()) return option_status;
          }
          return Status::Ok();
        }
      },
      expr->data);
}

// FROM/JOIN 中绑定的一个表。offset 是该表首列在连接输出行中的下标。
struct BoundTable {
  const TableInfo *table{nullptr};
  std::string table_name;
  std::string alias;
  std::size_t column_offset{0};

  [[nodiscard]] std::string_view VisibleName() const {
    return alias.empty() ? std::string_view(table_name) : std::string_view(alias);
  }
};

struct ResolvedTableColumn {
  std::size_t table_index{0};
  std::size_t column_index{0};
};

// 在全部已绑定表中解析列引用。限定名只接受该表实际暴露的可见名：
// 有 AS 时使用别名，否则使用真实表名。未限定同名列会明确报告歧义。
Result<ResolvedTableColumn> ResolveTableColumn(
    const CompileColumnExpr &column, const std::vector<BoundTable> &tables) {
  if (!column.table_name.empty()) {
    std::optional<ResolvedTableColumn> resolved;
    for (std::size_t table_index = 0; table_index < tables.size();
         ++table_index) {
      if (column.table_name != tables[table_index].VisibleName()) continue;
      if (resolved.has_value()) {
        return Result<ResolvedTableColumn>(Status::InvalidArgument(
            "JOIN 表限定符歧义: " + column.table_name));
      }
      auto column_index =
          tables[table_index].table->schema.FindColumnIndex(column.name);
      if (!column_index.ok()) {
        return Result<ResolvedTableColumn>(Status::NotFound(
            "JOIN 表 " + std::string(tables[table_index].VisibleName()) +
            " 中列不存在: " + column.name));
      }
      resolved = ResolvedTableColumn{table_index, column_index.value()};
    }
    if (!resolved.has_value()) {
      return Result<ResolvedTableColumn>(Status::NotFound(
          "JOIN 表限定符不存在: " + column.table_name));
    }
    return Result<ResolvedTableColumn>(*resolved);
  }

  std::optional<ResolvedTableColumn> resolved;
  for (std::size_t table_index = 0; table_index < tables.size();
       ++table_index) {
    auto column_index =
        tables[table_index].table->schema.FindColumnIndex(column.name);
    if (!column_index.ok()) continue;
    if (resolved.has_value()) {
      return Result<ResolvedTableColumn>(Status::InvalidArgument(
          "JOIN 列名歧义，请使用表名限定: " + column.name));
    }
    resolved = ResolvedTableColumn{table_index, column_index.value()};
  }
  if (!resolved.has_value()) {
    return Result<ResolvedTableColumn>(
        Status::NotFound("JOIN 列不存在: " + column.name));
  }
  return Result<ResolvedTableColumn>(*resolved);
}

}  // namespace

Result<Plan> Planner::Build(const Statement &statement, const CatalogReader &catalog) const {
  // 所有语句先完成语义检查和 Plan 构造。Planner 只读取 Catalog；创建表、
  // 写入记录等副作用统一由执行阶段负责。
  auto plan = std::visit(
      [&](const auto &value) -> Result<Plan> {
        using Type = std::decay_t<decltype(value)>;
        const auto fail_at = [&](Status status) -> Result<Plan> {
          // 为语义错误附加语句起始位置，同时保留原始错误码。
          return Result<Plan>(WithLocation(std::move(status), value.location));
        };
        if constexpr (std::is_same_v<Type, BeginStatement>) {
          return Result<Plan>(BeginPlan{});
        } else if constexpr (std::is_same_v<Type, CommitStatement>) {
          return Result<Plan>(CommitPlan{});
        } else if constexpr (std::is_same_v<Type, RollbackStatement>) {
          return Result<Plan>(RollbackPlan{});
        } else if constexpr (std::is_same_v<Type, CreateTableStatement>) {
          // 编译阶段只校验 Schema 和表名，真正登记发生在 CreateTable 执行器。
          auto schema_status = CheckSchema(value.schema);
          if (!schema_status.ok()) return fail_at(schema_status);
          auto existing = catalog.FindTable(value.table_name);
          if (existing.ok()) return fail_at(Status::AlreadyExists("表已存在: " + value.table_name));
          if (existing.status().code() != ErrorCode::NotFound) {
            return fail_at(existing.status());
          }
          return Result<Plan>(CreateTablePlan{value.table_name, value.schema});
        } else if constexpr (std::is_same_v<Type, InsertStatement>) {
          // INSERT 必须绑定到已有 Schema；显式列按目标列顺序检查，未指定列
          // 时按 Schema 定义顺序检查，并验证所有 VALUES 行。
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          auto values_status = NormalizeInsertRows(
              value.rows, value.columns, table.value()->schema);
          if (!values_status.ok()) return fail_at(values_status.status());
          return Result<Plan>(InsertPlan{
              value.table_name, value.columns,
              std::move(values_status.value())});
        } else if constexpr (std::is_same_v<Type, SelectStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());

          // JOIN 使用独立路径：按 SQL 顺序为每个子句解析 ON 两侧列，构造
          // 左深二元 JoinPlan，再把投影映射到连接行的组合列下标。
          if (!value.joins.empty()) {
            if (value.select_all == false &&
                value.projection_expressions.size() !=
                    value.projection.size()) {
              return fail_at(Status::InvalidArgument(
                  "JOIN 投影表达式数量与投影列数量不一致"));
            }
            if (value.where.has_value() || value.compile_where != nullptr ||
                !value.group_by.empty() || value.having != nullptr ||
                !value.order_by.empty()) {
              return fail_at(Status::NotImplemented(
                  "JOIN 当前仅支持 SELECT ... JOIN ... ON 等值连接"));
            }

            std::vector<BoundTable> tables;
            tables.push_back(
                BoundTable{table.value(), value.table_name, value.table_alias, 0});
            std::unordered_set<std::string> visible_names;
            visible_names.insert(
                std::string(tables.front().VisibleName()));

            std::shared_ptr<const PlanNode> root =
                std::make_shared<PlanNode>(SeqScanPlan{value.table_name});
            std::size_t next_column_offset = table.value()->schema.size();
            for (const auto &clause : value.joins) {
              auto right_table = RequireTable(catalog, clause.table_name);
              if (!right_table.ok()) return fail_at(right_table.status());

              BoundTable right_binding{right_table.value(), clause.table_name,
                                       clause.table_alias,
                                       next_column_offset};
              const std::string visible_name(
                  right_binding.VisibleName());
              if (!visible_names.insert(visible_name).second) {
                return fail_at(Status::AlreadyExists(
                    "JOIN 表名或别名重复: " + visible_name));
              }
              tables.push_back(right_binding);
              const std::size_t right_table_index = tables.size() - 1;

              auto condition_left = ResolveTableColumn(
                  CompileColumnExpr{clause.condition.left_table,
                                    clause.condition.left_column},
                  tables);
              if (!condition_left.ok()) return fail_at(condition_left.status());
              auto condition_right = ResolveTableColumn(
                  CompileColumnExpr{clause.condition.right_table,
                                    clause.condition.right_column},
                  tables);
              if (!condition_right.ok()) return fail_at(condition_right.status());

              const bool left_is_new =
                  condition_left.value().table_index == right_table_index;
              const bool right_is_new =
                  condition_right.value().table_index == right_table_index;
              if (left_is_new == right_is_new) {
                return fail_at(Status::InvalidArgument(
                    "JOIN ON 两侧必须分别来自累积结果和当前新表"));
              }

              const ResolvedTableColumn &prior_column =
                  left_is_new ? condition_right.value() : condition_left.value();
              const ResolvedTableColumn &new_column =
                  left_is_new ? condition_left.value() : condition_right.value();
              const auto &prior_schema = tables[prior_column.table_index].table->schema;
              const auto &new_schema =
                  tables[right_table_index].table->schema;
              const auto &prior_schema_column =
                  prior_schema.At(prior_column.column_index);
              const auto &new_schema_column =
                  new_schema.At(new_column.column_index);
              if (prior_schema_column.type != new_schema_column.type) {
                return fail_at(Status::TypeMismatch(
                    "JOIN ON 两侧列类型不一致"));
              }

              auto right_scan = std::make_shared<PlanNode>(
                  SeqScanPlan{clause.table_name});
              root = std::make_shared<PlanNode>(JoinPlan{
                  root, right_scan, clause.type,
                  std::string(
                      tables[prior_column.table_index].VisibleName()),
                  prior_schema_column.name,
                  std::string(tables[right_table_index].VisibleName()),
                  new_schema_column.name,
                  tables[prior_column.table_index].column_offset +
                      prior_column.column_index,
                  new_column.column_index});
              next_column_offset += new_schema.size();
            }

            std::vector<std::string> output_names;
            std::vector<PlanValueType> output_types;
            std::vector<std::size_t> input_indexes;
            if (value.select_all) {
              for (const auto &binding : tables) {
                for (const auto &column : binding.table->schema.columns()) {
                  output_names.push_back(column.name);
                  output_types.push_back(ToPlanValueType(column.type));
                }
              }
              input_indexes.resize(output_names.size());
              for (std::size_t i = 0; i < input_indexes.size(); ++i) {
                input_indexes[i] = i;
              }
            } else {
              std::unordered_set<std::string> projected;
              for (std::size_t i = 0; i < value.projection_expressions.size();
                   ++i) {
                const auto *column =
                    std::get_if<CompileColumnExpr>(
                        &value.projection_expressions[i]->data);
                if (column == nullptr) {
                  return fail_at(Status::NotImplemented(
                      "JOIN 当前只支持直接选择列"));
                }
                auto resolved = ResolveTableColumn(*column, tables);
                if (!resolved.ok()) return fail_at(resolved.status());
                const std::string output_name =
                    i < value.projection_aliases.size() &&
                            !value.projection_aliases[i].empty()
                        ? value.projection_aliases[i]
                        : column->name;
                if (!projected.insert(output_name).second) {
                  return fail_at(Status::AlreadyExists(
                      "JOIN 重复输出列: " + output_name));
                }
                output_names.push_back(output_name);
                const auto &output_column =
                    tables[resolved.value().table_index]
                        .table->schema.At(resolved.value().column_index);
                output_types.push_back(ToPlanValueType(output_column.type));
                input_indexes.push_back(
                    tables[resolved.value().table_index].column_offset +
                    resolved.value().column_index);
              }
            }

            std::shared_ptr<const PlanNode> project =
                std::make_shared<PlanNode>(ProjectPlan{
                    root, output_names, value.select_all, input_indexes});
            return Result<Plan>(SelectPlan{
                value.table_name, std::move(project), value.distinct,
                value.limit, value.projection_aliases, value.table_alias,
                value.projection_expressions, nullptr, false,
                std::move(output_types)});
          }

          if (!value.projection_aliases.empty() &&
              value.projection_aliases.size() != value.projection.size()) {
            return fail_at(Status::InvalidArgument(
                "SELECT 列别名数量与投影列数量不一致"));
          }
          if (!value.projection_expressions.empty() &&
              value.projection_expressions.size() !=
                  value.projection.size()) {
            return fail_at(Status::InvalidArgument(
                "SELECT 投影表达式数量与投影列数量不一致"));
          }
          if (value.compile_where != nullptr) {
            // SELECT 的复杂表达式已经接入执行；这里先完成限定符、列和类型
            // 检查。SELECT 列表中的未支持子查询会在后续构建阶段明确拒绝。
            auto qualifier_status = CheckSingleTableQualifiers(
                value.compile_where, value.table_name, value.table_alias,
                "WHERE");
            if (!qualifier_status.ok()) return fail_at(qualifier_status);
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE", false, &catalog);
            if (!where_type.ok()) return fail_at(where_type.status());
            if (where_type.value() != CompileType::Bool) {
              return fail_at(Status::TypeMismatch(
                  "WHERE 条件必须为 BOOL，实际 " +
                  CompileTypeName(where_type.value())));
            }
          }
          std::unordered_set<std::string> projected;
          std::vector<PlanValueType> output_types;
          if (value.select_all) {
            for (const auto &column : table.value()->schema.columns()) {
              output_types.push_back(ToPlanValueType(column.type));
            }
          }
          bool has_aggregates = false;
          // SELECT * 与显式列列表互斥；显式列或聚合表达式不能重复。
          if (value.select_all && !value.projection.empty()) {
            return fail_at(Status::InvalidArgument("SELECT * 不能与列名混用"));
          }
          for (std::size_t i = 0; i < value.projection.size(); ++i) {
            if (!projected.insert(value.projection[i]).second) {
              return fail_at(Status::AlreadyExists(
                  "SELECT 重复输出项: " + value.projection[i]));
            }
            const CompileExprPtr &expression =
                value.projection_expressions.empty()
                    ? nullptr
                    : value.projection_expressions[i];
            if (expression == nullptr) {
              return fail_at(Status::InvalidArgument(
                  "SELECT 投影缺少表达式: " + value.projection[i]));
            }
            auto qualifier_status = CheckSingleTableQualifiers(
                expression, value.table_name, value.table_alias,
                "SELECT " + value.projection[i]);
            if (!qualifier_status.ok()) return fail_at(qualifier_status);
            auto expression_type = CheckCompileExpr(
                expression, table.value()->schema,
                "SELECT " + value.projection[i], true, &catalog);
            if (!expression_type.ok()) return fail_at(expression_type.status());
            output_types.push_back(ToPlanValueType(expression_type.value()));
            if (ContainsAggregate(expression)) {
              has_aggregates = true;
            }
          }
          for (const auto &column_name : value.group_by) {
            // GROUP BY 只允许列名键；执行器按这些列构造分组边界。
            if (!table.value()->schema.FindColumn(column_name).ok()) {
              return fail_at(
                  Status::NotFound("GROUP BY 列不存在: " + column_name));
            }
          }
          for (const auto &key : value.order_by) {
            // ORDER BY 当前只支持列名键，不支持表达式或聚合函数。
            if (!table.value()->schema.FindColumn(key.column).ok()) {
              return fail_at(Status::NotFound("ORDER BY 列不存在: " + key.column));
            }
          }
          if (value.where.has_value()) {
            auto predicate_status = CheckPredicate(*value.where, table.value()->schema);
            if (!predicate_status.ok()) return fail_at(predicate_status);
          }
          if (value.having != nullptr) {
            if (value.group_by.empty()) {
              return fail_at(Status::InvalidArgument(
                  "HAVING 必须与 GROUP BY 一起使用"));
            }
            auto qualifier_status = CheckSingleTableQualifiers(
                value.having, value.table_name, value.table_alias, "HAVING");
            if (!qualifier_status.ok()) return fail_at(qualifier_status);
            auto having_type = CheckCompileExpr(
                value.having, table.value()->schema, "HAVING", true,
                &catalog);
            if (!having_type.ok()) return fail_at(having_type.status());
            if (having_type.value() != CompileType::Bool) {
              return fail_at(Status::TypeMismatch(
                  "HAVING 条件必须为 BOOL，实际 " +
                  CompileTypeName(having_type.value())));
            }
            if (ContainsAggregate(value.having)) has_aggregates = true;
          }
          if (has_aggregates || !value.group_by.empty() ||
              value.having != nullptr) {
            std::unordered_set<std::string> grouped(
                value.group_by.begin(), value.group_by.end());
            for (const auto &expression : value.projection_expressions) {
              auto grouped_status =
                  CheckGroupedExpr(expression, grouped, "SELECT");
              if (!grouped_status.ok()) return fail_at(grouped_status);
            }
            if (value.having != nullptr) {
              auto grouped_status =
                  CheckGroupedExpr(value.having, grouped, "HAVING");
              if (!grouped_status.ok()) return fail_at(grouped_status);
            }
          }
          // 从叶子向根构造：SeqScan -> Filter? -> GroupBy? -> OrderBy?
          // -> Project。根 Project 永不省略，Executor 依赖它获得输出列；
          // 随后的 Optimizer 可以在保持该形态的前提下把 SeqScan 改成
          // IndexScan，或在安全时裁剪冗余内层 Project。
          std::shared_ptr<const PlanNode> root = std::make_shared<PlanNode>(SeqScanPlan{value.table_name});
          if (value.compile_where != nullptr) {
            root = std::make_shared<PlanNode>(FilterPlan{
                root, value.where.value_or(Predicate{}), value.compile_where});
          }
          if (!value.group_by.empty()) {
            root = std::make_shared<PlanNode>(
                GroupByPlan{root, value.group_by});
          }
          if (!value.order_by.empty()) {
            root = std::make_shared<PlanNode>(
                OrderByPlan{root, value.order_by});
          }
          // 冻结计划形态：SELECT 的根算子固定为 Project，无 WHERE 时省略 Filter。
          std::vector<std::size_t> projection_indexes;
          if (!value.select_all) {
            for (const auto &expression : value.projection_expressions) {
              const auto *column =
                  expression
                      ? std::get_if<CompileColumnExpr>(&expression->data)
                      : nullptr;
              if (column == nullptr) {
                projection_indexes.clear();
                break;
              }
              auto index =
                  table.value()->schema.FindColumnIndex(column->name);
              if (!index.ok()) {
                projection_indexes.clear();
                break;
              }
              projection_indexes.push_back(index.value());
            }
          }
          root = std::make_shared<PlanNode>(
              ProjectPlan{root, value.projection, value.select_all,
                          std::move(projection_indexes)});
          // DISTINCT、LIMIT 和 AS 别名属于 SELECT 根计划元数据；DISTINCT
          // 和 LIMIT 由执行器在投影或聚合完成后应用，别名用于稳定输出列名。
          return Result<Plan>(SelectPlan{
              value.table_name, std::move(root), value.distinct, value.limit,
              value.projection_aliases, value.table_alias,
              value.projection_expressions, value.having, has_aggregates,
              std::move(output_types)});
        } else if constexpr (std::is_same_v<Type, DeleteStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.compile_where != nullptr) {
            // DELETE 的复杂条件也先做完整语义检查，再报告执行层未实现。
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE", false, &catalog);
            if (!where_type.ok()) return fail_at(where_type.status());
            if (where_type.value() != CompileType::Bool) {
              return fail_at(Status::TypeMismatch(
                  "WHERE 条件必须为 BOOL，实际 " +
                  CompileTypeName(where_type.value())));
            }
          }
          if (value.compile_where != nullptr && !value.where.has_value()) {
            return fail_at(Status::NotImplemented(
                "复杂 WHERE 表达式仅编译层支持，未接入数据库执行"));
          }
          if (value.where.has_value()) {
            auto predicate_status = CheckPredicate(*value.where, table.value()->schema);
            if (!predicate_status.ok()) return fail_at(predicate_status);
          }
          return Result<Plan>(DeletePlan{value.table_name, value.where});
        } else if constexpr (std::is_same_v<Type, DropTableStatement>) {
          // DROP 只验证目标表存在并生成计划，不在 Planner 中修改 Catalog；
          // 执行器随后按索引 -> 数据页 -> Catalog 记录的顺序完成删除。
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          return Result<Plan>(DropTablePlan{value.table_name});
        } else if constexpr (std::is_same_v<Type, AlterTableStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.action == AlterTableAction::RenameTable) {
            if (value.new_name.empty()) {
              return fail_at(
                  Status::InvalidArgument("ALTER TABLE 新表名不能为空"));
            }
            auto existing = catalog.FindTable(value.new_name);
            if (existing.ok()) {
              return fail_at(Status::AlreadyExists(
                  "ALTER TABLE 目标表已存在: " + value.new_name));
            }
            if (existing.status().code() != ErrorCode::NotFound) {
              return fail_at(existing.status());
            }
          } else if (value.action == AlterTableAction::RenameColumn) {
            auto old_column =
                table.value()->schema.FindColumn(value.column_name);
            if (!old_column.ok()) {
              return fail_at(Status::NotFound(
                  "ALTER TABLE 旧列不存在: " + value.column_name));
            }
            if (value.new_name.empty()) {
              return fail_at(
                  Status::InvalidArgument("ALTER TABLE 新列名不能为空"));
            }
            if (table.value()->schema.FindColumn(value.new_name).ok()) {
              return fail_at(Status::AlreadyExists(
                  "ALTER TABLE 新列名已存在: " + value.new_name));
            }
          } else {
            if (value.column.name.empty()) {
              return fail_at(
                  Status::InvalidArgument("ALTER TABLE 新列名不能为空"));
            }
            if (table.value()->schema.FindColumn(value.column.name).ok()) {
              return fail_at(Status::AlreadyExists(
                  "ALTER TABLE 新列已存在: " + value.column.name));
            }
            auto schema_status =
                CheckSchema(Schema(std::vector<Column>{value.column}));
            if (!schema_status.ok()) return fail_at(schema_status);
            if (value.default_value.has_value()) {
              auto default_status =
                  CheckInsertValue(*value.default_value, value.column,
                                   "ALTER TABLE DEFAULT ");
              if (!default_status.ok()) return fail_at(default_status);
            }
          }
          return Result<Plan>(AlterTablePlan{
              value.table_name, value.action, value.new_name,
              value.column_name, value.column, value.default_value});
        } else if constexpr (std::is_same_v<Type, CreateIndexStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          auto column = table.value()->schema.FindColumn(value.column_name);
          if (!column.ok()) {
            return fail_at(Status::NotFound(
                "索引列不存在: " + value.column_name));
          }
          if (column.value()->type != DataType::Int) {
            return fail_at(Status::TypeMismatch(
                "当前索引只支持 INT 列: " + value.column_name));
          }
          return Result<Plan>(CreateIndexPlan{
              value.index_name, value.table_name, value.column_name,
              value.is_unique});
        } else if constexpr (std::is_same_v<Type, DropIndexStatement>) {
          // 索引是否存在由执行阶段 IndexManager::DropIndex 统一检查。
          return Result<Plan>(DropIndexPlan{value.index_name});
        } else if constexpr (std::is_same_v<Type, ExplainStatement>) {
          auto inner = Build(value.select, catalog);
          if (!inner.ok()) return fail_at(inner.status());
          auto *select = std::get_if<SelectPlan>(&inner.value());
          if (select == nullptr) {
            return fail_at(Status::InternalError(
                "EXPLAIN 内部必须是 SELECT"));
          }
          return Result<Plan>(ExplainPlan{*select});
        } else {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.compile_where != nullptr) {
            // UPDATE WHERE 与 SELECT WHERE 使用相同的类型检查规则。
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE", false, &catalog);
            if (!where_type.ok()) return fail_at(where_type.status());
            if (where_type.value() != CompileType::Bool) {
              return fail_at(Status::TypeMismatch(
                  "WHERE 条件必须为 BOOL，实际 " +
                  CompileTypeName(where_type.value())));
            }
          }
          if (value.compile_where != nullptr && !value.where.has_value()) {
            return fail_at(Status::NotImplemented(
                "复杂 WHERE 表达式仅编译层支持，未接入数据库执行"));
          }
          std::unordered_set<std::string> updated_columns;
          for (const auto &assignment : value.assignments) {
            // 同一列不能在一条 UPDATE 中重复赋值。
            if (!updated_columns.insert(assignment.column).second) {
              return fail_at(Status::AlreadyExists(
                  "UPDATE SET 重复列: " + assignment.column));
            }
            auto column = table.value()->schema.FindColumn(assignment.column);
            if (!column.ok()) {
              return fail_at(Status::NotFound(
                  "UPDATE SET 列不存在: " + assignment.column));
            }
            auto expression_type =
                CheckCompileExpr(assignment.expression, table.value()->schema,
                                 "UPDATE SET " + assignment.column, false,
                                 &catalog);
            if (!expression_type.ok()) {
              return fail_at(expression_type.status());
            }
            const CompileType expected =
                column.value()->type == DataType::Int ? CompileType::Int
                                                      : CompileType::Varchar;
            // SET 右值类型必须与目标列一致。
            if (expression_type.value() != expected) {
              return fail_at(Status::TypeMismatch(
                  "UPDATE SET 类型不匹配: " + assignment.column +
                  "，期望 " + CompileTypeName(expected) + "，实际 " +
                  CompileTypeName(expression_type.value())));
            }
            if (column.value()->type == DataType::Varchar &&
                column.value()->length.has_value() &&
                assignment.value.has_value() &&
                assignment.value->AsVarchar().size() >
                    *column.value()->length) {
              return fail_at(Status::InvalidArgument(
                  "UPDATE SET 字符串超过列长度: " + assignment.column));
            }
          }
          if (value.where.has_value()) {
            auto predicate_status =
                CheckPredicate(*value.where, table.value()->schema);
            if (!predicate_status.ok()) return fail_at(predicate_status);
          }
          // 生成结构化 UpdatePlan。执行器按简单 Predicate 选行，逐行计算
          // SET 表达式，并同步维护 Heap 和该表上的索引。
          return Result<Plan>(
              UpdatePlan{value.table_name, value.assignments, value.where});
        }
      },
      statement);
  if (!plan.ok()) return plan;
  // 编译层后处理：只允许不改变冻结计划形态的重写规则（见 optimizer.h）。
  // SELECT 计划额外携带目标表索引元数据，供索引扫描选择使用。
  std::vector<IndexMetadata> indexes;
  if (auto *select = std::get_if<SelectPlan>(&plan.value())) {
    indexes = catalog.ListTableIndexes(select->table_name);
  }
  return Optimizer().Optimize(std::move(plan.value()), indexes);
}

// 逻辑计划的稳定文本输出，只服务调试和测试，不作为 Executor 的输入格式。
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
          std::string result =
              "InsertPlan(table=" + value.table_name + ",columns=";
          if (value.columns.empty()) {
            result += "*";
          } else {
            result += "[";
            for (std::size_t i = 0; i < value.columns.size(); ++i) {
              if (i != 0) result += ",";
              result += value.columns[i];
            }
            result += "]";
          }
          result += ",rows=[";
          for (std::size_t row_index = 0; row_index < value.rows.size();
               ++row_index) {
            if (row_index != 0) result += ",";
            result += "[";
            for (std::size_t i = 0; i < value.rows[row_index].size(); ++i) {
              if (i != 0) result += ",";
            const auto &insert_value = value.rows[row_index][i];
            if (insert_value.IsNull()) {
              result += "NULL";
            } else {
              result += insert_value.IsInt()
                            ? insert_value.ToString()
                            : "'" + insert_value.AsVarchar() + "'";
            }
            }
            result += "]";
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, SelectPlan>) {
          std::string result =
              "SelectPlan(table=" + value.table_name +
              ",root=" + NodeText(value.root);
          if (!value.table_alias.empty()) {
            result += ",alias=" + value.table_alias;
          }
          if (value.distinct) result += ",distinct=true";
          if (value.limit.has_value()) {
            result += ",limit=" + std::to_string(*value.limit);
          }
          bool has_alias = false;
          for (const auto &alias : value.projection_aliases) {
            if (!alias.empty()) {
              has_alias = true;
              break;
            }
          }
          if (has_alias) {
            result += ",aliases=[";
            for (std::size_t i = 0; i < value.projection_aliases.size(); ++i) {
              if (i != 0) result += ",";
              result += value.projection_aliases[i];
            }
            result += "]";
          }
          if (value.has_aggregates) result += ",aggregates=true";
          if (value.having != nullptr) {
            result += ",having=" + CompileExprText(value.having);
          }
          return result + ")";
        } else if constexpr (std::is_same_v<Type, DeletePlan>) {
          std::string result = "DeletePlan(table=" + value.table_name;
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          return result + ")";
        } else if constexpr (std::is_same_v<Type, DropTablePlan>) {
          return "DropTablePlan(table=" + value.table_name + ")";
        } else if constexpr (std::is_same_v<Type, AlterTablePlan>) {
          std::string action =
              value.action == AlterTableAction::RenameTable
                  ? "rename_table"
                  : value.action == AlterTableAction::RenameColumn
                        ? "rename_column"
                        : "add_column";
          std::string result = "AlterTablePlan(table=" + value.table_name +
                               ",action=" + action;
          if (value.action == AlterTableAction::RenameColumn) {
            result += ",column=" + value.column_name;
          } else if (value.action == AlterTableAction::AddColumn) {
            result += ",column=" + value.column.name + ":" +
                      (value.column.type == DataType::Int ? "INT"
                                                         : "VARCHAR");
          }
          if (value.action != AlterTableAction::AddColumn) {
            result += ",new_name=" + value.new_name;
          }
          if (value.default_value.has_value()) {
            result += ",default=";
            if (value.default_value->IsNull()) {
              result += "NULL";
            } else {
              result += value.default_value->IsInt()
                            ? value.default_value->ToString()
                            : "'" + value.default_value->AsVarchar() + "'";
            }
          }
          return result + ")";
        } else if constexpr (std::is_same_v<Type, BeginPlan>) {
          return "BeginPlan()";
        } else if constexpr (std::is_same_v<Type, CommitPlan>) {
          return "CommitPlan()";
        } else if constexpr (std::is_same_v<Type, RollbackPlan>) {
          return "RollbackPlan()";
        } else if constexpr (std::is_same_v<Type, CreateIndexPlan>) {
          return "CreateIndexPlan(index=" + value.index_name +
                 ",table=" + value.table_name +
                 ",column=" + value.column_name +
                 ",unique=" + (value.is_unique ? "true" : "false") + ")";
        } else if constexpr (std::is_same_v<Type, DropIndexPlan>) {
          return "DropIndexPlan(index=" + value.index_name + ")";
        } else if constexpr (std::is_same_v<Type, ExplainPlan>) {
          return "ExplainPlan(select=" + ToString(value.select) + ")";
        } else {
          std::string result = "UpdatePlan(table=" + value.table_name +
                               ",assignments=[";
          for (std::size_t i = 0; i < value.assignments.size(); ++i) {
            if (i != 0) result += ",";
            result += value.assignments[i].column + "=" +
                      CompileExprText(value.assignments[i].expression);
          }
          result += "]";
          if (value.where.has_value()) {
            result += ",where=" + PredicateText(*value.where);
          }
          return result + ")";
        }
      },
      plan);
}

}  // namespace oursql
