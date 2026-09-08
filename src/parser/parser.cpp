#include "oursql/parser/parser.h"

#include <charconv>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

std::string QuoteValue(const Value &value) {
  if (value.IsInt()) return value.ToString();
  return "'" + value.AsVarchar() + "'";
}

std::string SchemaText(const Schema &schema) {
  std::string result = "[";
  for (std::size_t i = 0; i < schema.size(); ++i) {
    if (i != 0) result += ",";
    const auto &column = schema.At(i);
    result += column.name + ":" + (column.type == DataType::Int ? "INT" : "VARCHAR");
    if (column.length.has_value()) result += "(" + std::to_string(*column.length) + ")";
  }
  return result + "]";
}

std::string PredicateText(const Predicate &predicate) {
  return predicate.column + "=" + QuoteValue(predicate.value);
}

class ParserImpl {
 public:
  explicit ParserImpl(const std::vector<Token> &tokens) : tokens_(tokens) {}

  Result<std::vector<Statement>> Parse() {
    std::vector<Statement> statements;
    while (Peek().type != TokenType::EndOfFile) {
      auto statement = ParseStatement();
      if (!statement.ok()) return Result<std::vector<Statement>>(statement.status());
      statements.push_back(std::move(statement.value()));
    }
    if (statements.empty()) return Result<std::vector<Statement>>(Error("statement", Peek()));
    return Result<std::vector<Statement>>(std::move(statements));
  }

 private:
  const Token &Peek() const { return tokens_[index_]; }
  const Token &Previous() const { return tokens_[index_ - 1]; }

  bool Match(TokenType type) {
    if (Peek().type != type) return false;
    ++index_;
    return true;
  }

  Status Error(std::string expected, const Token &actual) const {
    std::string actual_text = TokenTypeName(actual.type);
    if (!actual.lexeme.empty()) actual_text += "('" + actual.lexeme + "')";
    return Status::InvalidArgument("语法错误：期望 " + std::move(expected) + "，实际 " + actual_text +
                                   "，位置 " + std::to_string(actual.line) + ":" +
                                   std::to_string(actual.column));
  }

  Result<Token> Expect(TokenType type, std::string expected) {
    if (Peek().type != type) return Result<Token>(Error(std::move(expected), Peek()));
    return Result<Token>(tokens_[index_++]);
  }

  Result<std::string> ExpectIdentifier(std::string expected) {
    auto token = Expect(TokenType::Identifier, std::move(expected));
    if (!token.ok()) return Result<std::string>(token.status());
    return Result<std::string>(std::move(token.value().lexeme));
  }

  Result<Value> ParseLiteral() {
    if (Match(TokenType::String)) return Result<Value>(Value(Previous().lexeme));
    if (Match(TokenType::Integer)) {
      const auto &text = Previous().lexeme;
      std::int64_t value = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        return Result<Value>(Status::InvalidArgument("整数超出范围，位置 " +
                                                     std::to_string(Previous().line) + ":" +
                                                     std::to_string(Previous().column)));
      }
      return Result<Value>(Value(value));
    }
    return Result<Value>(Error("整数或单引号字符串", Peek()));
  }

  Result<Predicate> ParsePredicate() {
    auto column = ExpectIdentifier("WHERE 后的列名");
    if (!column.ok()) return Result<Predicate>(column.status());
    auto equal = Expect(TokenType::Equal, "'='");
    if (!equal.ok()) return Result<Predicate>(equal.status());
    auto value = ParseLiteral();
    if (!value.ok()) return Result<Predicate>(value.status());
    return Result<Predicate>(Predicate(std::move(column.value()), std::move(value.value())));
  }

  Result<Statement> ParseCreateTable() {
    auto create = Expect(TokenType::Create, "CREATE");
    if (!create.ok()) return Result<Statement>(create.status());
    auto table = Expect(TokenType::Table, "TABLE");
    if (!table.ok()) return Result<Statement>(table.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    auto left = Expect(TokenType::LeftParen, "'('");
    if (!left.ok()) return Result<Statement>(left.status());

    std::vector<Column> columns;
    while (true) {
      auto column_name = ExpectIdentifier("列名");
      if (!column_name.ok()) return Result<Statement>(column_name.status());
      DataType type;
      std::optional<std::size_t> length;
      if (Match(TokenType::Int)) {
        type = DataType::Int;
      } else if (Match(TokenType::Varchar)) {
        type = DataType::Varchar;
        if (Match(TokenType::LeftParen)) {
          auto length_token = Expect(TokenType::Integer, "VARCHAR 的长度");
          if (!length_token.ok()) return Result<Statement>(length_token.status());
          std::size_t parsed_length = 0;
          const auto &text = length_token.value().lexeme;
          const auto parsed = std::from_chars(text.data(), text.data() + text.size(), parsed_length);
          if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || parsed_length == 0) {
            return Result<Statement>(Status::InvalidArgument("VARCHAR 长度非法，位置 " +
                                                               std::to_string(length_token.value().line) + ":" +
                                                               std::to_string(length_token.value().column)));
          }
          length = parsed_length;
          auto right_length = Expect(TokenType::RightParen, "')'");
          if (!right_length.ok()) return Result<Statement>(right_length.status());
        }
      } else {
        return Result<Statement>(Error("INT 或 VARCHAR", Peek()));
      }
      columns.emplace_back(std::move(column_name.value()), type, length);
      if (!Match(TokenType::Comma)) break;
    }
    auto right = Expect(TokenType::RightParen, "')'");
    if (!right.ok()) return Result<Statement>(right.status());
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(CreateTableStatement{std::move(table_name.value()), Schema(std::move(columns))});
  }

  Result<Statement> ParseInsert() {
    auto insert = Expect(TokenType::Insert, "INSERT");
    if (!insert.ok()) return Result<Statement>(insert.status());
    auto into = Expect(TokenType::Into, "INTO");
    if (!into.ok()) return Result<Statement>(into.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    auto values = Expect(TokenType::Values, "VALUES");
    if (!values.ok()) return Result<Statement>(values.status());
    auto left = Expect(TokenType::LeftParen, "'('");
    if (!left.ok()) return Result<Statement>(left.status());

    std::vector<Value> literals;
    while (true) {
      auto literal = ParseLiteral();
      if (!literal.ok()) return Result<Statement>(literal.status());
      literals.push_back(std::move(literal.value()));
      if (!Match(TokenType::Comma)) break;
    }
    auto right = Expect(TokenType::RightParen, "')'");
    if (!right.ok()) return Result<Statement>(right.status());
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(InsertStatement{std::move(table_name.value()), std::move(literals)});
  }

  Result<Statement> ParseSelect() {
    auto select = Expect(TokenType::Select, "SELECT");
    if (!select.ok()) return Result<Statement>(select.status());
    SelectStatement statement;
    if (Match(TokenType::Star)) {
      statement.select_all = true;
    } else {
      auto first = ExpectIdentifier("SELECT 后的列名或 '*'");
      if (!first.ok()) return Result<Statement>(first.status());
      statement.projection.push_back(std::move(first.value()));
      while (Match(TokenType::Comma)) {
        auto column = ExpectIdentifier("逗号后的列名");
        if (!column.ok()) return Result<Statement>(column.status());
        statement.projection.push_back(std::move(column.value()));
      }
    }
    auto from = Expect(TokenType::From, "FROM");
    if (!from.ok()) return Result<Statement>(from.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    statement.table_name = std::move(table_name.value());
    if (Match(TokenType::Where)) {
      auto predicate = ParsePredicate();
      if (!predicate.ok()) return Result<Statement>(predicate.status());
      statement.where = std::move(predicate.value());
    }
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseDelete() {
    auto delete_keyword = Expect(TokenType::Delete, "DELETE");
    if (!delete_keyword.ok()) return Result<Statement>(delete_keyword.status());
    auto from = Expect(TokenType::From, "FROM");
    if (!from.ok()) return Result<Statement>(from.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    DeleteStatement statement;
    statement.table_name = std::move(table_name.value());
    if (Match(TokenType::Where)) {
      auto predicate = ParsePredicate();
      if (!predicate.ok()) return Result<Statement>(predicate.status());
      statement.where = std::move(predicate.value());
    }
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseStatement() {
    switch (Peek().type) {
      case TokenType::Create: return ParseCreateTable();
      case TokenType::Insert: return ParseInsert();
      case TokenType::Select: return ParseSelect();
      case TokenType::Delete: return ParseDelete();
      default: return Result<Statement>(Error("CREATE、INSERT、SELECT 或 DELETE", Peek()));
    }
  }

  const std::vector<Token> &tokens_;
  std::size_t index_{0};
};

}  // namespace

Result<std::vector<Statement>> Parser::Parse(std::string_view sql) const {
  Lexer lexer;
  auto tokens = lexer.Tokenize(sql);
  if (!tokens.ok()) return Result<std::vector<Statement>>(tokens.status());
  return ParserImpl(tokens.value()).Parse();
}

std::string ToString(const Statement &statement) {
  return std::visit(
      [](const auto &value) -> std::string {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CreateTableStatement>) {
          return "CreateTableStatement(table=" + value.table_name + ",schema=" + SchemaText(value.schema) + ")";
        } else if constexpr (std::is_same_v<Type, InsertStatement>) {
          std::string result = "InsertStatement(table=" + value.table_name + ",values=[";
          for (std::size_t i = 0; i < value.values.size(); ++i) {
            if (i != 0) result += ",";
            result += QuoteValue(value.values[i]);
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, SelectStatement>) {
          std::string result = "SelectStatement(table=" + value.table_name + ",projection=";
          if (value.select_all) {
            result += "*";
          } else {
            result += "[";
            for (std::size_t i = 0; i < value.projection.size(); ++i) {
              if (i != 0) result += ",";
              result += value.projection[i];
            }
            result += "]";
          }
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          return result + ")";
        } else {
          std::string result = "DeleteStatement(table=" + value.table_name;
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          return result + ")";
        }
      },
      statement);
}

}  // namespace oursql
