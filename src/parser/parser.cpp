#include "oursql/parser/parser.h"
#include "oursql/optimizer/expression_optimizer.h"

#include <charconv>
#include <functional>
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

Position TokenPosition(const Token &token) noexcept {
  return Position{token.start_offset, token.line, token.column};
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

  CompileExprPtr MakeColumnExpr(std::string name) {
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(CompileColumnExpr{std::move(name)}));
  }

  CompileExprPtr MakeLiteralExpr(const Value &value) {
    CompileLiteralExpr literal;
    if (value.IsInt()) {
      literal.kind = CompileLiteralKind::Int;
      literal.integer = value.AsInt();
    } else {
      literal.kind = CompileLiteralKind::String;
      literal.text = value.AsVarchar();
    }
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(std::move(literal)));
  }

  CompileExprPtr MakeBoolExpr(bool value) {
    CompileLiteralExpr literal;
    literal.kind = CompileLiteralKind::Bool;
    literal.text = value ? "true" : "false";
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(std::move(literal)));
  }

  CompileExprPtr MakeBinary(std::string op, CompileExprPtr left,
                            CompileExprPtr right) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileBinaryExpr{std::move(op), std::move(left), std::move(right)}));
  }

  CompileExprPtr MakeUnary(std::string op, CompileExprPtr operand) {
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(CompileUnaryExpr{std::move(op), std::move(operand)}));
  }

  static bool IsComparisonType(TokenType type) {
    return type == TokenType::Equal || type == TokenType::NotEqual ||
           type == TokenType::Greater || type == TokenType::GreaterEqual ||
           type == TokenType::Less || type == TokenType::LessEqual;
  }

  static bool IsAdditiveType(TokenType type) {
    return type == TokenType::Plus || type == TokenType::Minus;
  }

  static bool IsMultiplicativeType(TokenType type) {
    return type == TokenType::Star || type == TokenType::Slash;
  }

  Result<CompileExprPtr> ParseCompileExpr() { return ParseCompileOr(); }

  Result<CompileExprPtr> ParseCompileOr() {
    auto left = ParseCompileAnd();
    if (!left.ok()) return left;
    while (Match(TokenType::Or)) {
      auto right = ParseCompileAnd();
      if (!right.ok()) return right;
      left.value() = MakeBinary("or", left.value(), right.value());
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileAnd() {
    auto left = ParseCompileNot();
    if (!left.ok()) return left;
    while (Match(TokenType::And)) {
      auto right = ParseCompileNot();
      if (!right.ok()) return right;
      left.value() = MakeBinary("and", left.value(), right.value());
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileNot() {
    if (Match(TokenType::Not)) {
      auto operand = ParseCompileNot();
      if (!operand.ok()) return operand;
      return Result<CompileExprPtr>(MakeUnary("not", operand.value()));
    }
    return ParseCompileComparison();
  }

  Result<CompileExprPtr> ParseCompileComparison() {
    auto left = ParseCompileAdditive();
    if (!left.ok()) return left;
    if (IsComparisonType(Peek().type)) {
      auto operation = Expect(Peek().type, "比较运算符");
      if (!operation.ok()) return Result<CompileExprPtr>(operation.status());
      auto right = ParseCompileAdditive();
      if (!right.ok()) return right;
      left.value() = MakeBinary(operation.value().lexeme, left.value(),
                                right.value());
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileAdditive() {
    auto left = ParseCompileMultiplicative();
    if (!left.ok()) return left;
    while (IsAdditiveType(Peek().type)) {
      auto operation = Expect(Peek().type, "加减运算符");
      if (!operation.ok()) return Result<CompileExprPtr>(operation.status());
      auto right = ParseCompileMultiplicative();
      if (!right.ok()) return right;
      left.value() = MakeBinary(operation.value().lexeme, left.value(),
                                right.value());
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileMultiplicative() {
    auto left = ParseCompileUnary();
    if (!left.ok()) return left;
    while (IsMultiplicativeType(Peek().type)) {
      auto operation = Expect(Peek().type, "乘除运算符");
      if (!operation.ok()) return Result<CompileExprPtr>(operation.status());
      auto right = ParseCompileUnary();
      if (!right.ok()) return right;
      left.value() = MakeBinary(operation.value().lexeme, left.value(),
                                right.value());
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileUnary() {
    if (Match(TokenType::Minus)) {
      auto operand = ParseCompileUnary();
      if (!operand.ok()) return operand;
      return Result<CompileExprPtr>(MakeUnary("-", operand.value()));
    }
    return ParseCompilePrimary();
  }

  Result<CompileExprPtr> ParseCompilePrimary() {
    if (Match(TokenType::True)) {
      return Result<CompileExprPtr>(MakeBoolExpr(true));
    }
    if (Match(TokenType::False)) {
      return Result<CompileExprPtr>(MakeBoolExpr(false));
    }
    if (Match(TokenType::String)) {
      return Result<CompileExprPtr>(
          MakeLiteralExpr(Value(Previous().lexeme)));
    }
    if (Match(TokenType::Integer)) {
      const auto &text = Previous().lexeme;
      std::int64_t number = 0;
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), number);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != text.data() + text.size()) {
        return Result<CompileExprPtr>(
            Status::InvalidArgument("整数超出范围，位置 " +
                                    std::to_string(Previous().line) + ":" +
                                    std::to_string(Previous().column)));
      }
      return Result<CompileExprPtr>(MakeLiteralExpr(Value(number)));
    }
    if (Peek().type == TokenType::Identifier) {
      auto name = ExpectIdentifier("表达式中的列名");
      if (!name.ok()) return Result<CompileExprPtr>(name.status());
      return Result<CompileExprPtr>(MakeColumnExpr(std::move(name.value())));
    }
    if (Match(TokenType::LeftParen)) {
      auto inner = ParseCompileExpr();
      if (!inner.ok()) return inner;
      auto right = Expect(TokenType::RightParen, "')'");
      if (!right.ok()) return Result<CompileExprPtr>(right.status());
      return inner;
    }
    return Result<CompileExprPtr>(
        Error("表达式（列名、常量、括号或 NOT）", Peek()));
  }

  std::optional<Predicate> ExtractPredicate(const CompileExprPtr &expr) {
    return ExtractPredicateFromCompileExpr(expr);
  }

  std::optional<Value> ExtractLiteral(const CompileExprPtr &expr) {
    return ExtractLiteralValue(expr);
  }

  struct ParsedWhere {
    CompileExprPtr expression;
    std::optional<Predicate> predicate;
  };

  Result<ParsedWhere> ParseWhere() {
    auto expression = ParseCompileExpr();
    if (!expression.ok()) return Result<ParsedWhere>(expression.status());
    auto folded = FoldCompileExpr(expression.value());
    return Result<ParsedWhere>(
        ParsedWhere{folded, ExtractPredicate(folded)});
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
    CreateTableStatement statement{std::move(table_name.value()),
                                   Schema(std::move(columns))};
    statement.location = TokenPosition(create.value());
    return Result<Statement>(std::move(statement));
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
    InsertStatement statement{std::move(table_name.value()),
                              std::move(literals)};
    statement.location = TokenPosition(insert.value());
    return Result<Statement>(std::move(statement));
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
    statement.location = TokenPosition(select.value());
    if (Match(TokenType::Where)) {
      auto parsed_where = ParseWhere();
      if (!parsed_where.ok()) return Result<Statement>(parsed_where.status());
      statement.where = std::move(parsed_where.value().predicate);
      statement.compile_where =
          std::move(parsed_where.value().expression);
    }
    if (Match(TokenType::Group)) {
      auto by = Expect(TokenType::By, "BY after GROUP");
      if (!by.ok()) return Result<Statement>(by.status());
      do {
        auto column = ExpectIdentifier("GROUP BY 后的列名");
        if (!column.ok()) return Result<Statement>(column.status());
        statement.group_by.push_back(std::move(column.value()));
        if (!Match(TokenType::Comma)) break;
      } while (true);
    }
    if (Match(TokenType::Order)) {
      auto by = Expect(TokenType::By, "BY after ORDER");
      if (!by.ok()) return Result<Statement>(by.status());
      do {
        OrderKey key;
        auto column = ExpectIdentifier("ORDER BY 后的列名");
        if (!column.ok()) return Result<Statement>(column.status());
        key.column = std::move(column.value());
        if (Match(TokenType::Desc)) {
          key.direction = OrderDirection::Desc;
        } else if (Match(TokenType::Asc)) {
          key.direction = OrderDirection::Asc;
        }
        statement.order_by.push_back(std::move(key));
        if (!Match(TokenType::Comma)) break;
      } while (true);
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
    statement.location = TokenPosition(delete_keyword.value());
    if (Match(TokenType::Where)) {
      auto parsed_where = ParseWhere();
      if (!parsed_where.ok()) return Result<Statement>(parsed_where.status());
      statement.where = std::move(parsed_where.value().predicate);
      statement.compile_where =
          std::move(parsed_where.value().expression);
    }
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseUpdate() {
    auto update = Expect(TokenType::Update, "UPDATE");
    if (!update.ok()) return Result<Statement>(update.status());
    auto table_name = ExpectIdentifier("UPDATE 后的表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    auto set = Expect(TokenType::Set, "SET");
    if (!set.ok()) return Result<Statement>(set.status());

    UpdateStatement statement;
    statement.table_name = std::move(table_name.value());
    statement.location = TokenPosition(update.value());
    do {
      auto column = ExpectIdentifier("SET 后的列名");
      if (!column.ok()) return Result<Statement>(column.status());
      auto equal = Expect(TokenType::Equal, "'='");
      if (!equal.ok()) return Result<Statement>(equal.status());
      auto expression = ParseCompileExpr();
      if (!expression.ok()) return Result<Statement>(expression.status());
      auto folded = FoldCompileExpr(expression.value());
      statement.assignments.push_back(
          UpdateAssignment{std::move(column.value()),
                           folded,
                           ExtractLiteral(folded)});
      if (!Match(TokenType::Comma)) break;
    } while (true);

    if (Match(TokenType::Where)) {
      auto parsed_where = ParseWhere();
      if (!parsed_where.ok()) return Result<Statement>(parsed_where.status());
      statement.where = std::move(parsed_where.value().predicate);
      statement.compile_where =
          std::move(parsed_where.value().expression);
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
      case TokenType::Update: return ParseUpdate();
      default: return Result<Statement>(
          Error("CREATE、INSERT、SELECT、DELETE 或 UPDATE", Peek()));
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
          if (value.compile_where != nullptr && !value.where.has_value()) {
            result += ",where_expr=" + CompileExprText(value.compile_where);
          }
          if (!value.group_by.empty()) {
            result += ",group_by=[";
            for (std::size_t i = 0; i < value.group_by.size(); ++i) {
              if (i != 0) result += ",";
              result += value.group_by[i];
            }
            result += "]";
          }
          if (!value.order_by.empty()) {
            result += ",order_by=[";
            for (std::size_t i = 0; i < value.order_by.size(); ++i) {
              if (i != 0) result += ",";
              result += value.order_by[i].column;
              result += value.order_by[i].direction == OrderDirection::Desc
                            ? " DESC"
                            : " ASC";
            }
            result += "]";
          }
          return result + ")";
        } else if constexpr (std::is_same_v<Type, DeleteStatement>) {
          std::string result = "DeleteStatement(table=" + value.table_name;
          if (value.where.has_value()) result += ",where=" + PredicateText(*value.where);
          if (value.compile_where != nullptr && !value.where.has_value()) {
            result += ",where_expr=" + CompileExprText(value.compile_where);
          }
          return result + ")";
        } else {
          std::string result = "UpdateStatement(table=" + value.table_name +
                               ",assignments=[";
          for (std::size_t i = 0; i < value.assignments.size(); ++i) {
            if (i != 0) result += ",";
            result += value.assignments[i].column + "=" +
                      CompileExprText(value.assignments[i].expression);
          }
          result += "]";
          if (value.where.has_value()) {
            result += ",where=" + PredicateText(*value.where);
          } else if (value.compile_where != nullptr) {
            result += ",where_expr=" + CompileExprText(value.compile_where);
          }
          return result + ")";
        }
      },
      statement);
}

}  // namespace oursql
