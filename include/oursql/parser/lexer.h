#pragma once

#include "oursql/common/status.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace oursql {

enum class TokenType {
  Identifier,
  Integer,
  String,
  Comma,
  LeftParen,
  RightParen,
  Star,
  Equal,
  Semicolon,
  Create,
  Table,
  Insert,
  Into,
  Values,
  Select,
  From,
  Where,
  Delete,
  Int,
  Varchar,
  EndOfFile,
};

struct Token {
  TokenType type{TokenType::EndOfFile};
  std::string lexeme;
  std::size_t start_offset{0};
  std::size_t end_offset{0};
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
