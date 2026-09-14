#include "oursql/parser/lexer.h"

#include <cctype>

// Lexer 实现说明：
//
// 扫描顺序为空白 -> 注释 -> 标识符/关键字 -> 数字 -> 字符串 -> 运算符/
// 分隔符 -> 非法字符。每次消费字符都会同步维护 offset、line、column；
// Token 使用左闭右开区间，并记录结束行列。
//
// 数字只支持 INT；遇到小数格式会整体收集后报告非法数字。字符串支持两个
// 连续单引号转义；字符串内容保持原样，关键字和标识符统一转小写。
namespace oursql {

namespace {

// SQL 关键字和当前项目的标识符都按 ASCII 大小写不敏感处理；字符串字面量不会
// 走这个函数，因此其中内容仍保持原始大小写。
char LowerAscii(char character) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
}

// 将已经转成小写的标识符映射为关键字 Token。未命中的词保留为 Identifier，
// 因此表名、列名不需要维护单独的保留字列表。
TokenType KeywordType(std::string_view word) {
  if (word.compare("create") == 0) return TokenType::Create;
  if (word.compare("table") == 0) return TokenType::Table;
  if (word.compare("index") == 0) return TokenType::Index;
  if (word.compare("unique") == 0) return TokenType::Unique;
  if (word.compare("on") == 0) return TokenType::On;
  if (word.compare("insert") == 0) return TokenType::Insert;
  if (word.compare("into") == 0) return TokenType::Into;
  if (word.compare("values") == 0) return TokenType::Values;
  if (word.compare("select") == 0) return TokenType::Select;
  if (word.compare("explain") == 0) return TokenType::Explain;
  if (word.compare("distinct") == 0) return TokenType::Distinct;
  if (word.compare("from") == 0) return TokenType::From;
  if (word.compare("where") == 0) return TokenType::Where;
  if (word.compare("is") == 0) return TokenType::Is;
  if (word.compare("between") == 0) return TokenType::Between;
  if (word.compare("in") == 0) return TokenType::In;
  if (word.compare("like") == 0) return TokenType::Like;
  if (word.compare("group") == 0) return TokenType::Group;
  if (word.compare("having") == 0) return TokenType::Having;
  if (word.compare("order") == 0) return TokenType::Order;
  if (word.compare("by") == 0) return TokenType::By;
  if (word.compare("limit") == 0) return TokenType::Limit;
  if (word.compare("asc") == 0) return TokenType::Asc;
  if (word.compare("desc") == 0) return TokenType::Desc;
  if (word.compare("as") == 0) return TokenType::As;
  if (word.compare("exists") == 0) return TokenType::Exists;
  if (word.compare("join") == 0) return TokenType::Join;
  if (word.compare("inner") == 0) return TokenType::Inner;
  if (word.compare("left") == 0) return TokenType::Left;
  if (word.compare("right") == 0) return TokenType::Right;
  if (word.compare("full") == 0) return TokenType::Full;
  if (word.compare("outer") == 0) return TokenType::Outer;
  if (word.compare("delete") == 0) return TokenType::Delete;
  if (word.compare("drop") == 0) return TokenType::Drop;
  if (word.compare("alter") == 0) return TokenType::Alter;
  if (word.compare("add") == 0) return TokenType::Add;
  if (word.compare("column") == 0) return TokenType::Column;
  if (word.compare("rename") == 0) return TokenType::Rename;
  if (word.compare("to") == 0) return TokenType::To;
  if (word.compare("default") == 0) return TokenType::Default;
  if (word.compare("primary") == 0) return TokenType::Primary;
  if (word.compare("key") == 0) return TokenType::Key;
  if (word.compare("begin") == 0) return TokenType::Begin;
  if (word.compare("commit") == 0) return TokenType::Commit;
  if (word.compare("rollback") == 0) return TokenType::Rollback;
  if (word.compare("update") == 0) return TokenType::Update;
  if (word.compare("set") == 0) return TokenType::Set;
  if (word.compare("int") == 0) return TokenType::Int;
  if (word.compare("varchar") == 0) return TokenType::Varchar;
  if (word.compare("and") == 0) return TokenType::And;
  if (word.compare("or") == 0) return TokenType::Or;
  if (word.compare("not") == 0) return TokenType::Not;
  if (word.compare("true") == 0) return TokenType::True;
  if (word.compare("false") == 0) return TokenType::False;
  if (word.compare("null") == 0) return TokenType::Null;
  return TokenType::Identifier;
}

}  // namespace

const char *TokenTypeName(TokenType type) noexcept {
  switch (type) {
    case TokenType::Identifier: return "identifier";
    case TokenType::Integer: return "integer";
    case TokenType::String: return "string";
    case TokenType::True: return "TRUE";
    case TokenType::False: return "FALSE";
    case TokenType::Null: return "NULL";
    case TokenType::Comma: return "','";
    case TokenType::Dot: return "'.'";
    case TokenType::LeftParen: return "'('";
    case TokenType::RightParen: return "')'";
    case TokenType::Star: return "'*'";
    case TokenType::Equal: return "'='";
    case TokenType::NotEqual: return "'!='";
    case TokenType::Greater: return "'>'";
    case TokenType::GreaterEqual: return "'>='";
    case TokenType::Less: return "'<'";
    case TokenType::LessEqual: return "'<='";
    case TokenType::Plus: return "'+'";
    case TokenType::Minus: return "'-'";
    case TokenType::Slash: return "'/'";
    case TokenType::And: return "AND";
    case TokenType::Or: return "OR";
    case TokenType::Not: return "NOT";
    case TokenType::Semicolon: return "';'";
    case TokenType::Create: return "CREATE";
    case TokenType::Table: return "TABLE";
    case TokenType::Index: return "INDEX";
    case TokenType::Unique: return "UNIQUE";
    case TokenType::On: return "ON";
    case TokenType::Insert: return "INSERT";
    case TokenType::Into: return "INTO";
    case TokenType::Values: return "VALUES";
    case TokenType::Select: return "SELECT";
    case TokenType::Explain: return "EXPLAIN";
    case TokenType::Distinct: return "DISTINCT";
    case TokenType::From: return "FROM";
    case TokenType::Where: return "WHERE";
    case TokenType::Is: return "IS";
    case TokenType::Between: return "BETWEEN";
    case TokenType::In: return "IN";
    case TokenType::Like: return "LIKE";
    case TokenType::Group: return "GROUP";
    case TokenType::Having: return "HAVING";
    case TokenType::Order: return "ORDER";
    case TokenType::By: return "BY";
    case TokenType::Limit: return "LIMIT";
    case TokenType::Asc: return "ASC";
    case TokenType::Desc: return "DESC";
    case TokenType::As: return "AS";
    case TokenType::Exists: return "EXISTS";
    case TokenType::Join: return "JOIN";
    case TokenType::Inner: return "INNER";
    case TokenType::Left: return "LEFT";
    case TokenType::Right: return "RIGHT";
    case TokenType::Full: return "FULL";
    case TokenType::Outer: return "OUTER";
    case TokenType::Delete: return "DELETE";
    case TokenType::Drop: return "DROP";
    case TokenType::Alter: return "ALTER";
    case TokenType::Add: return "ADD";
    case TokenType::Column: return "COLUMN";
    case TokenType::Rename: return "RENAME";
    case TokenType::To: return "TO";
    case TokenType::Default: return "DEFAULT";
    case TokenType::Primary: return "PRIMARY";
    case TokenType::Key: return "KEY";
    case TokenType::Begin: return "BEGIN";
    case TokenType::Commit: return "COMMIT";
    case TokenType::Rollback: return "ROLLBACK";
    case TokenType::Update: return "UPDATE";
    case TokenType::Set: return "SET";
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

  // 更新逻辑行列。换行后列回到 1；原始字节偏移另由 offset 单独维护，
  // 因而 Token 可以同时支持定点诊断和按字节切片。
  const auto advance = [&](char character, std::size_t *current_line,
                           std::size_t *current_column) {
    if (character == '\n') {
      ++*current_line;
      *current_column = 1;
    } else {
      ++*current_column;
    }
  };

  // 所有 Token 都通过该函数生成，保证起止偏移和行列字段的填充方式一致。
  // end_offset 指向 Token 消耗完成后的位置，即采用左闭右开区间。
  const auto make_token = [&](TokenType type, std::size_t start, std::size_t start_line,
                              std::size_t start_column, std::size_t end, std::size_t end_line,
                              std::size_t end_column, std::string lexeme) {
    tokens.push_back(Token{type, std::move(lexeme), start, end, start_line, start_column,
                           end_line, end_column});
  };

  // 主循环只消费 offset 指向的字符。每个分支必须保证 offset 前进，避免非法
  // 输入导致 Lexer 死循环。
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

    // 行注释：-- 到行尾，负数仍以 -digit 形式保留。
    if (character == '-' && offset + 1 < sql.size() && sql[offset + 1] == '-') {
      while (offset < sql.size() && sql[offset] != '\n' && sql[offset] != '\r') {
        advance(sql[offset], &line, &column);
        ++offset;
      }
      continue;
    }
    // 块注释：/* ... */，允许跨行。
    if (character == '/' && offset + 1 < sql.size() && sql[offset + 1] == '*') {
      const auto comment_line = start_line;
      const auto comment_column = start_column;
      advance(sql[offset], &line, &column);
      ++offset;
      advance(sql[offset], &line, &column);
      ++offset;
      bool closed = false;
      while (offset < sql.size()) {
        if (sql[offset] == '*' && offset + 1 < sql.size() &&
            sql[offset + 1] == '/') {
          advance(sql[offset], &line, &column);
          ++offset;
          advance(sql[offset], &line, &column);
          ++offset;
          closed = true;
          break;
        }
        advance(sql[offset], &line, &column);
        ++offset;
      }
      if (!closed) {
        return Result<std::vector<Token>>(Status::InvalidArgument(
            "未闭合块注释，位置 " + std::to_string(comment_line) + ":" +
            std::to_string(comment_column)));
      }
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
      std::string word;
      // 标识符首字符为 ASCII 字母或下划线，后续允许数字。写入 word 时统一
      // 转小写，从而让 SQL 名称解析与关键字解析保持同一套大小写规则。
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

    if (std::isdigit(static_cast<unsigned char>(character)) ||
        (character == '-' && offset + 1 < sql.size() &&
         std::isdigit(static_cast<unsigned char>(sql[offset + 1])))) {
      // 负数与数字合并成一个 Integer Token；例如 -12 的 lexeme 为 "-12"。
      // 项目当前数值类型只有 INT，不把小数点视为合法数字组成部分；遇到
      // 1.2.3 一类输入会在这里整体收集并报告非法数字格式。
      std::string number;
      if (character == '-') {
        number.push_back(character);
        advance(character, &line, &column);
        ++offset;
      }
      while (offset < sql.size() && std::isdigit(static_cast<unsigned char>(sql[offset]))) {
        number.push_back(sql[offset]);
        advance(sql[offset], &line, &column);
        ++offset;
      }
      if (offset + 1 < sql.size() && sql[offset] == '.' &&
          std::isdigit(static_cast<unsigned char>(sql[offset + 1]))) {
        while (offset < sql.size() &&
               (std::isdigit(static_cast<unsigned char>(sql[offset])) ||
                sql[offset] == '.')) {
          number.push_back(sql[offset]);
          advance(sql[offset], &line, &column);
          ++offset;
        }
        return Result<std::vector<Token>>(Status::InvalidArgument(
            "非法数字格式 '" + number + "'，位置 " +
            std::to_string(start_line) + ":" + std::to_string(start_column)));
      }
      make_token(TokenType::Integer, start, start_line, start_column, offset, line, column,
                 std::move(number));
      continue;
    }

    if (character == '\'') {
      // 字符串内容不写入 Token.lexeme 的引号本身；连续两个单引号表示一个
      // 转义单引号，例如 'It''s' 的 lexeme 是 It's。
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

    // 先匹配双字符运算符，避免把 >=、<=、!=、==、<> 拆成两个 Token。
    if ((character == '>' || character == '<' || character == '!' ||
         character == '=') &&
        offset + 1 < sql.size() && sql[offset + 1] == '=') {
      TokenType pair_type = TokenType::EndOfFile;
      if (character == '>' && sql[offset + 1] == '=') pair_type = TokenType::GreaterEqual;
      if (character == '<' && sql[offset + 1] == '=') pair_type = TokenType::LessEqual;
      if (character == '!' && sql[offset + 1] == '=') pair_type = TokenType::NotEqual;
      if (character == '=' && sql[offset + 1] == '=') pair_type = TokenType::Equal;
      std::string pair;
      pair.push_back(character);
      pair.push_back(sql[offset + 1]);
      advance(sql[offset], &line, &column);
      advance(sql[offset + 1], &line, &column);
      offset += 2;
      make_token(pair_type, start, start_line, start_column, offset, line,
                 column, std::move(pair));
      continue;
    }
    if (character == '<' && offset + 1 < sql.size() &&
        sql[offset + 1] == '>') {
      advance(sql[offset], &line, &column);
      advance(sql[offset + 1], &line, &column);
      offset += 2;
      make_token(TokenType::NotEqual, start, start_line, start_column, offset,
                 line, column, "<>");
      continue;
    }

    // 剩余合法符号均为单字符标点或运算符。
    TokenType punctuation = TokenType::EndOfFile;
    switch (character) {
      case ',': punctuation = TokenType::Comma; break;
      case '.': punctuation = TokenType::Dot; break;
      case '(': punctuation = TokenType::LeftParen; break;
      case ')': punctuation = TokenType::RightParen; break;
      case '*': punctuation = TokenType::Star; break;
      case '=': punctuation = TokenType::Equal; break;
      case '>': punctuation = TokenType::Greater; break;
      case '<': punctuation = TokenType::Less; break;
      case '!': punctuation = TokenType::NotEqual; break;
      case '+': punctuation = TokenType::Plus; break;
      case '-': punctuation = TokenType::Minus; break;
      case '/': punctuation = TokenType::Slash; break;
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

  // EOF 不占用源码字符，start/end 均指向输入末尾；Parser 依赖该哨兵安全结束。
  tokens.push_back(Token{TokenType::EndOfFile, "", offset, offset, line, column, line, column});
  return Result<std::vector<Token>>(std::move(tokens));
}

}  // namespace oursql
