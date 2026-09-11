#pragma once

#include "oursql/common/status.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace oursql {

enum class TokenType {
  // 标识符和字面量：Lexer 会将关键字与标识符转为小写，但字符串内容保持原样。
  Identifier,
  Integer,
  String,
  True,
  False,

  // 标点、比较运算和算术运算。
  Comma,
  LeftParen,
  RightParen,
  Star,
  Equal,
  NotEqual,
  Greater,
  GreaterEqual,
  Less,
  LessEqual,
  Plus,
  Minus,
  Slash,
  And,
  Or,
  Not,

  // 语句分隔符和 SQL 关键字。
  Semicolon,
  Create,
  Table,
  Index,
  Unique,
  On,
  Insert,
  Into,
  Values,
  Select,
  Explain,
  Distinct,
  From,
  Where,
  Between,
  In,
  Like,
  Group,
  Having,
  Order,
  By,
  Limit,
  Asc,
  Desc,
  As,
  Delete,
  Drop,
  Update,
  Set,
  Int,
  Varchar,
  EndOfFile,
};

struct Token {
  // 词法种别；EOF 是输入末尾的哨兵，Parser 依赖它避免越过 vector 边界。
  TokenType type{TokenType::EndOfFile};

  // 规范化后的词素：关键字和标识符为小写，字符串保存去除外层引号后的内容。
  std::string lexeme;

  // [start_offset, end_offset) 是 Token 在原始 SQL 中占用的字节区间。
  std::size_t start_offset{0};
  std::size_t end_offset{0};

  // 起始行列从 1 开始；结束位置表示 Token 消耗完后的下一个位置。
  std::size_t line{1};
  std::size_t column{1};
  std::size_t end_line{1};
  std::size_t end_column{1};
};

// 调用者：Parser 或调试工具；作用：把 token 类型转换为稳定名称；返回：名称文本。
[[nodiscard]] const char *TokenTypeName(TokenType type) noexcept;

class Lexer {
 public:
  // 调用者：SQL 编译管线；作用：将 SQL 文本转换为带位置的 token；返回：token 序列或词法错误。
  [[nodiscard]] Result<std::vector<Token>> Tokenize(std::string_view sql) const;
};

}  // namespace oursql
