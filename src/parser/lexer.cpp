#include "oursql/parser/lexer.h"

#include <cctype>

namespace oursql {

namespace {

char LowerAscii(char character) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

TokenType KeywordType(std::string_view word) {
  if (word.compare("create") == 0) return TokenType::Create;
  if (word.compare("table") == 0) return TokenType::Table;
  if (word.compare("insert") == 0) return TokenType::Insert;
  if (word.compare("into") == 0) return TokenType::Into;
  if (word.compare("values") == 0) return TokenType::Values;
  if (word.compare("select") == 0) return TokenType::Select;
  if (word.compare("from") == 0) return TokenType::From;
  if (word.compare("where") == 0) return TokenType::Where;
  if (word.compare("delete") == 0) return TokenType::Delete;
  if (word.compare("int") == 0) return TokenType::Int;
  if (word.compare("varchar") == 0) return TokenType::Varchar;
  return TokenType::Identifier;
}

}  // namespace

const char *TokenTypeName(TokenType type) noexcept {
  switch (type) {
    case TokenType::Identifier: return "identifier";
    case TokenType::Integer: return "integer";
    case TokenType::String: return "string";
    case TokenType::Comma: return "','";
    case TokenType::LeftParen: return "'('";
    case TokenType::RightParen: return "')'";
    case TokenType::Star: return "'*'";
    case TokenType::Equal: return "'='";
    case TokenType::Semicolon: return "';'";
    case TokenType::Create: return "CREATE";
    case TokenType::Table: return "TABLE";
    case TokenType::Insert: return "INSERT";
    case TokenType::Into: return "INTO";
    case TokenType::Values: return "VALUES";
    case TokenType::Select: return "SELECT";
    case TokenType::From: return "FROM";
    case TokenType::Where: return "WHERE";
    case TokenType::Delete: return "DELETE";
    case TokenType::Int: return "INT";
    case TokenType::Varchar: return "VARCHAR";
    case TokenType::EndOfFile: return "EOF";
  }
  return "unknown";
}

Result<std::vector<Token>> Lexer::Tokenize(std::string_view sql) const {
  std::vector<Token> tokens;
  std::size_t offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;

  const auto advance = [&](char character, std::size_t *current_line,
                           std::size_t *current_column) {
    if (character == '\n') {
      ++*current_line;
      *current_column = 1;
    } else {
      ++*current_column;
    }
  };
  const auto make_token = [&](TokenType type, std::size_t start, std::size_t start_line,
                              std::size_t start_column, std::size_t end, std::size_t end_line,
                              std::size_t end_column, std::string lexeme) {
    tokens.push_back(Token{type, std::move(lexeme), start, end, start_line, start_column,
                           end_line, end_column});
  };

  while (offset < sql.size()) {
    const char character = sql[offset];
    if (std::isspace(static_cast<unsigned char>(character))) {
      advance(character, &line, &column);
      ++offset;
      continue;
    }

    const auto start = offset;
    const auto start_line = line;
    const auto start_column = column;
    if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
      std::string word;
      while (offset < sql.size() &&
             (std::isalnum(static_cast<unsigned char>(sql[offset])) || sql[offset] == '_')) {
        word.push_back(LowerAscii(sql[offset]));
        advance(sql[offset], &line, &column);
        ++offset;
      }
      const auto token_type = KeywordType(word);
      make_token(token_type, start, start_line, start_column, offset, line, column, std::move(word));
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(character))) {
      std::string number;
      while (offset < sql.size() && std::isdigit(static_cast<unsigned char>(sql[offset]))) {
        number.push_back(sql[offset]);
        advance(sql[offset], &line, &column);
        ++offset;
      }
      make_token(TokenType::Integer, start, start_line, start_column, offset, line, column,
                 std::move(number));
      continue;
    }

    if (character == '\'') {
      advance(character, &line, &column);
      ++offset;
      std::string value;
      bool closed = false;
      while (offset < sql.size()) {
        const char current = sql[offset];
        if (current == '\'') {
          if (offset + 1 < sql.size() && sql[offset + 1] == '\'') {
            value.push_back('\'');
            advance(sql[offset], &line, &column);
            advance(sql[offset + 1], &line, &column);
            offset += 2;
            continue;
          }
          advance(current, &line, &column);
          ++offset;
          closed = true;
          break;
        }
        value.push_back(current);
        advance(current, &line, &column);
        ++offset;
      }
      if (!closed) {
        return Result<std::vector<Token>>(Status::InvalidArgument(
            "未闭合字符串，位置 " + std::to_string(start_line) + ":" +
            std::to_string(start_column)));
      }
      make_token(TokenType::String, start, start_line, start_column, offset, line, column,
                 std::move(value));
      continue;
    }

    TokenType punctuation = TokenType::EndOfFile;
    switch (character) {
      case ',': punctuation = TokenType::Comma; break;
      case '(': punctuation = TokenType::LeftParen; break;
      case ')': punctuation = TokenType::RightParen; break;
      case '*': punctuation = TokenType::Star; break;
      case '=': punctuation = TokenType::Equal; break;
      case ';': punctuation = TokenType::Semicolon; break;
      default:
        return Result<std::vector<Token>>(Status::InvalidArgument(
            "非法字符 '" + std::string(1, character) + "'，位置 " +
            std::to_string(start_line) + ":" + std::to_string(start_column)));
    }
    advance(character, &line, &column);
    ++offset;
    make_token(punctuation, start, start_line, start_column, offset, line, column,
               std::string(1, character));
  }

  tokens.push_back(Token{TokenType::EndOfFile, "", offset, offset, line, column, line, column});
  return Result<std::vector<Token>>(std::move(tokens));
}

}  // namespace oursql
