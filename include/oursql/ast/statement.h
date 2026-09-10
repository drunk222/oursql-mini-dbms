#pragma once

#include "oursql/common/schema.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace oursql {

// 源码位置：offset 与 token 起始偏移一致，行列从 1 开始。
// 调用者：Lexer/Parser 与编译层诊断；作用：为 AST 节点记录出错位置。
struct Position {
  std::size_t offset{0};
  std::size_t line{0};
  std::size_t column{0};
};

struct Predicate {
  std::string column;
  Value value;

  Predicate() = default;

  // 调用者：解析器；作用：描述一个等值条件；返回：列名和值。
  Predicate(std::string column_name, Value literal) : column(std::move(column_name)), value(std::move(literal)) {}
};

// 编译层表达式：仅供解析与打印，不进入 Catalog/Executor。
enum class CompileLiteralKind { Int, String, Bool };

struct CompileExpr;
using CompileExprPtr = std::shared_ptr<const CompileExpr>;

struct CompileColumnExpr {
  std::string name;
};

struct CompileLiteralExpr {
  CompileLiteralKind kind{CompileLiteralKind::Int};
  std::int64_t integer{0};
  std::string text;
};

struct CompileBinaryExpr {
  std::string op;
  CompileExprPtr left;
  CompileExprPtr right;
};

struct CompileUnaryExpr {
  std::string op;
  CompileExprPtr operand;
};

struct CompileExpr {
  using Data = std::variant<CompileColumnExpr, CompileLiteralExpr,
                            CompileBinaryExpr, CompileUnaryExpr>;
  explicit CompileExpr(Data expression_data)
      : data(std::move(expression_data)) {}
  Data data;
};

enum class OrderDirection { Asc, Desc };

struct OrderKey {
  std::string column;
  OrderDirection direction{OrderDirection::Asc};
};

struct CreateTableStatement {
  std::string table_name;
  Schema schema;
  Position location;
};

struct InsertStatement {
  std::string table_name;
  std::vector<Value> values;
  Position location;
};

struct SelectStatement {
  std::string table_name;
  std::vector<std::string> projection;
  bool select_all{false};
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  std::vector<std::string> group_by;
  std::vector<OrderKey> order_by;
  Position location;
};

struct DeleteStatement {
  std::string table_name;
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  Position location;
};

struct UpdateAssignment {
  std::string column;
  CompileExprPtr expression;
  std::optional<Value> value;
};

struct UpdateStatement {
  std::string table_name;
  std::vector<UpdateAssignment> assignments;
  std::optional<Predicate> where;
  CompileExprPtr compile_where;
  Position location;
};

using Statement = std::variant<CreateTableStatement, InsertStatement,
                                SelectStatement, DeleteStatement,
                                UpdateStatement>;

// 调用者：调试器或测试；作用：以稳定格式打印 AST；返回：AST 文本，不参与执行。
[[nodiscard]] std::string ToString(const Statement &statement);

}  // namespace oursql
