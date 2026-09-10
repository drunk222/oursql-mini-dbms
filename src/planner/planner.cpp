#include "oursql/planner/planner.h"

#include "oursql/optimizer/optimizer.h"

#include <functional>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace oursql {

namespace {

// 检查已经抽取出的数据库层 Predicate。这里只处理“列 = 字面量”的简单形态，
// 复杂表达式由 CheckCompileExpr() 递归检查。
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

// CREATE TABLE 的静态 Schema 检查：至少一列、列名唯一、长度属性只用于
// VARCHAR，并拒绝零长度。Catalog 是否已有同名表在 Planner::Build 中检查。
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

// 检查 INSERT 值列表。列数决定 VALUES 数量；每个位置都必须和对应列类型
// 一致，且受 VARCHAR(n) 的字节长度限制。
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

// 以下文本打印函数只用于测试和调试，不参与执行或再次解析。
std::string PredicateText(const Predicate &predicate) {
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
          return value.text;
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          return "(" + CompileExprText(value.left) + " " + value.op + " " +
                 CompileExprText(value.right) + ")";
        } else {
          return "(" + value.op + " " + CompileExprText(value.operand) + ")";
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
        } else if constexpr (std::is_same_v<Type, FilterPlan>) {
          return "FilterPlan(predicate=" + PredicateText(operation.predicate) +
                 ",child=" + NodeText(operation.child) + ")";
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

enum class CompileType { Int, Varchar, Bool, Invalid };

// Invalid 只用于保持 switch 完整性；正常检查会直接返回 Status 错误。
std::string CompileTypeName(CompileType type) {
  switch (type) {
    case CompileType::Int: return "INT";
    case CompileType::Varchar: return "VARCHAR";
    case CompileType::Bool: return "BOOL";
    case CompileType::Invalid: return "INVALID";
  }
  return "INVALID";
}

// 递归推导 CompileExpr 的结果类型，同时检查列是否存在和运算符操作数类型。
// context 用于把错误定位到 WHERE、UPDATE SET 等具体语法位置。
Result<CompileType> CheckCompileExpr(const CompileExprPtr &expr,
                                     const Schema &schema,
                                     const std::string &context) {
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
          // 编译器内部区分 INT、VARCHAR 和 BOOL 三种字面量类型。
          if (value.kind == CompileLiteralKind::Int) {
            return Result<CompileType>(CompileType::Int);
          }
          if (value.kind == CompileLiteralKind::String) {
            return Result<CompileType>(CompileType::Varchar);
          }
          return Result<CompileType>(CompileType::Bool);
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          // NOT 要求 BOOL 操作数，一元负号只允许 INT。
          auto operand = CheckCompileExpr(value.operand, schema, context);
          if (!operand.ok()) return operand;
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
        } else {
          // 二元表达式先递归检查左右子树，再组合并检查结果类型。
          auto left = CheckCompileExpr(value.left, schema, context);
          if (!left.ok()) return left;
          auto right = CheckCompileExpr(value.right, schema, context);
          if (!right.ok()) return right;
          if (value.op == "and" || value.op == "or") {
            if (left.value() != CompileType::Bool ||
                right.value() != CompileType::Bool) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": " + value.op + " 需要 BOOL 操作数"));
            }
            return Result<CompileType>(CompileType::Bool);
          }
          if (value.op == "+" || value.op == "-" || value.op == "*" ||
              value.op == "/") {
            if (left.value() != CompileType::Int ||
                right.value() != CompileType::Int) {
              return Result<CompileType>(Status::TypeMismatch(
                  context + ": 算术运算需要 INT 操作数"));
            }
            return Result<CompileType>(CompileType::Int);
          }
          if (value.op == ">" || value.op == ">=" || value.op == "<" ||
              value.op == "<=" || value.op == "=" || value.op == "!=" ||
              value.op == "<>") {
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
        if constexpr (std::is_same_v<Type, CreateTableStatement>) {
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
          // INSERT 必须绑定到已有 Schema，逐项检查数量和类型。
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          auto values_status = CheckValues(value.values, table.value()->schema);
          if (!values_status.ok()) return fail_at(values_status);
          return Result<Plan>(InsertPlan{value.table_name, value.values});
        } else if constexpr (std::is_same_v<Type, SelectStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.compile_where != nullptr) {
            // 先完成表达式列和类型检查，再判断执行层是否支持该形态。
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE");
            if (!where_type.ok()) return fail_at(where_type.status());
            if (where_type.value() != CompileType::Bool) {
              return fail_at(Status::TypeMismatch(
                  "WHERE 条件必须为 BOOL，实际 " +
                  CompileTypeName(where_type.value())));
            }
          }
          if (value.compile_where != nullptr && !value.where.has_value()) {
            // 复杂条件没有通用执行算子时必须显式拒绝，不能静默忽略过滤。
            return fail_at(Status::NotImplemented(
                "复杂 WHERE 表达式仅编译层支持，未接入数据库执行"));
          }
          std::unordered_set<std::string> projected;
          // SELECT * 与显式列列表互斥；显式列不能重复且必须存在。
          if (value.select_all && !value.projection.empty()) {
            return fail_at(Status::InvalidArgument("SELECT * 不能与列名混用"));
          }
          for (const auto &column_name : value.projection) {
            if (!projected.insert(column_name).second) {
              return fail_at(Status::AlreadyExists("SELECT 重复列: " + column_name));
            }
            if (!table.value()->schema.FindColumn(column_name).ok()) {
              return fail_at(Status::NotFound("SELECT 列不存在: " + column_name));
            }
          }
          for (const auto &column_name : value.group_by) {
            // GROUP BY 先检查列存在，执行能力由 ExecutionEngine 明确拒绝。
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
          // 从叶子向根构造：SeqScan -> Filter? -> GroupBy? -> OrderBy?
          // -> Project。根 Project 永不省略，Executor 依赖它获得输出列。
          std::shared_ptr<const PlanNode> root = std::make_shared<PlanNode>(SeqScanPlan{value.table_name});
          if (value.where.has_value()) root = std::make_shared<PlanNode>(FilterPlan{root, *value.where});
          if (!value.group_by.empty()) {
            root = std::make_shared<PlanNode>(
                GroupByPlan{root, value.group_by});
          }
          if (!value.order_by.empty()) {
            root = std::make_shared<PlanNode>(
                OrderByPlan{root, value.order_by});
          }
          // 冻结计划形态：SELECT 的根算子固定为 Project，无 WHERE 时省略 Filter。
          root = std::make_shared<PlanNode>(
              ProjectPlan{root, value.projection, value.select_all});
          return Result<Plan>(SelectPlan{value.table_name, std::move(root)});
        } else if constexpr (std::is_same_v<Type, DeleteStatement>) {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.compile_where != nullptr) {
            // DELETE 的复杂条件也先做完整语义检查，再报告执行层未实现。
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE");
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
        } else {
          auto table = RequireTable(catalog, value.table_name);
          if (!table.ok()) return fail_at(table.status());
          if (value.compile_where != nullptr) {
            // UPDATE WHERE 与 SELECT WHERE 使用相同的类型检查规则。
            auto where_type =
                CheckCompileExpr(value.compile_where, table.value()->schema,
                                 "WHERE");
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
                                 "UPDATE SET " + assignment.column);
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
          // 生成结构化 UpdatePlan；执行层会明确返回 NotImplemented，
          // 因此计划本身只承担编译层表达与诊断职责。
          return Result<Plan>(
              UpdatePlan{value.table_name, value.assignments, value.where});
        }
      },
      statement);
  if (!plan.ok()) return plan;
  // 编译层后处理：只允许不改变冻结计划形态的重写规则（见 optimizer.h）。
  return Optimizer().Optimize(std::move(plan.value()));
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
          std::string result = "InsertPlan(table=" + value.table_name + ",values=[";
          for (std::size_t i = 0; i < value.values.size(); ++i) {
            if (i != 0) result += ",";
            result += value.values[i].IsInt() ? value.values[i].ToString()
                                              : "'" + value.values[i].AsVarchar() + "'";
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, SelectPlan>) {
          return "SelectPlan(table=" + value.table_name + ",root=" + NodeText(value.root) + ")";
        } else if constexpr (std::is_same_v<Type, DeletePlan>) {
          std::string result = "DeletePlan(table=" + value.table_name;
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          return result + ")";
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
