#pragma once

#include "oursql/ast/statement.h"
#include "oursql/parser/lexer.h"

#include <string_view>
#include <vector>

namespace oursql {

class Parser {
 public:
  // 标识符规则：SQL 关键字不区分大小写；表名和列名统一转换为 ASCII 小写，字符串值保持原样。
  // 调用者：命令行入口或上层控制器；作用：把 SQL 文本解析为 AST；返回：AST 序列或语法错误。
  // Parser 不查询 Catalog、不访问存储、不执行 SQL；表/列/类型检查由 Planner 负责。
  [[nodiscard]] Result<std::vector<Statement>> Parse(std::string_view sql) const;
};

}  // namespace oursql
