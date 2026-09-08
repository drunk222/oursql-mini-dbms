#pragma once

#include "oursql/common/schema.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace oursql {

struct Predicate {
  std::string column;
  Value value;

  Predicate() = default;

  // 调用者：解析器；作用：描述一个等值条件；返回：列名和值。
  Predicate(std::string column_name, Value literal) : column(std::move(column_name)), value(std::move(literal)) {}
};

struct CreateTableStatement {
  std::string table_name;
  Schema schema;
};

struct InsertStatement {
  std::string table_name;
  std::vector<Value> values;
};

struct SelectStatement {
  std::string table_name;
  std::vector<std::string> projection;
  bool select_all{false};
  std::optional<Predicate> where;
};

struct DeleteStatement {
  std::string table_name;
  std::optional<Predicate> where;
};

using Statement = std::variant<CreateTableStatement, InsertStatement, SelectStatement, DeleteStatement>;

// 调用者：调试器或测试；作用：以稳定格式打印 AST；返回：AST 文本，不参与执行。
[[nodiscard]] std::string ToString(const Statement &statement);

}  // namespace oursql
