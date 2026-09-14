#pragma once

#include "oursql/ast/statement.h"
#include "oursql/parser/lexer.h"

#include <string_view>
#include <vector>

namespace oursql {

class Parser {
 public:
  // 调用者：DatabaseEngine/命令行控制器。
  // 输入：可能包含多条 SQL 的 UTF-8 文本或 ASCII SQL。
  // 处理：先调用 Lexer，再用手写递归下降按语句边界构造强类型 AST。
  // 返回：按输入顺序排列的 Statement，或包含期望 Token、实际 Token 和行列
  // 位置的语法错误；空输入会返回错误，而不是产生空计划。
  //
  // 标识符规则：SQL 关键字不区分大小写；表名和列名统一转换为 ASCII 小写，
  // 字符串内容保持原样。Parser 不查询 Catalog、不访问存储、不执行 SQL；表、
  // 列、类型、INSERT 行数和约束合法性由 Planner 负责。
  [[nodiscard]] Result<std::vector<Statement>> Parse(std::string_view sql) const;
};

}  // namespace oursql
