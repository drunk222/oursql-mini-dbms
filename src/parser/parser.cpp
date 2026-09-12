#include "oursql/parser/parser.h"
#include "oursql/optimizer/expression_optimizer.h"

#include <charconv>
#include <functional>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

// 以下格式化函数只用于 AST 的稳定文本输出。它们不解析 SQL、不访问数据库，
// 测试和答辩展示可以依赖其格式，但执行链路不能依赖文本结果。
std::string QuoteValue(const Value &value) {
  if (value.IsNull()) return "NULL";
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
  if (predicate.kind == PredicateKind::IsNull) {
    return predicate.column + " is null";
  }
  if (predicate.kind == PredicateKind::IsNotNull) {
    return predicate.column + " is not null";
  }
  return predicate.column + "=" + QuoteValue(predicate.value);
}

std::string AggregateName(CompileAggregateKind kind) {
  switch (kind) {
    case CompileAggregateKind::Count: return "count";
    case CompileAggregateKind::Sum: return "sum";
    case CompileAggregateKind::Avg: return "avg";
    case CompileAggregateKind::Max: return "max";
    case CompileAggregateKind::Min: return "min";
  }
  return "unknown";
}

std::string JoinTypeName(JoinType type) {
  switch (type) {
    case JoinType::Inner: return "inner";
    case JoinType::Left: return "left";
    case JoinType::Right: return "right";
    case JoinType::Full: return "full";
  }
  return "unknown";
}

std::optional<CompileAggregateKind> AggregateKindFromName(
    std::string_view name) {
  if (name == "count") return CompileAggregateKind::Count;
  if (name == "sum") return CompileAggregateKind::Sum;
  if (name == "avg") return CompileAggregateKind::Avg;
  if (name == "max") return CompileAggregateKind::Max;
  if (name == "min") return CompileAggregateKind::Min;
  return std::nullopt;
}

std::string CompileExprText(const CompileExprPtr &expr) {
  if (expr == nullptr) return "<null>";
  return std::visit(
      [&](const auto &value) -> std::string {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          return value.table_name.empty()
                     ? value.name
                     : value.table_name + "." + value.name;
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          if (value.kind == CompileLiteralKind::Int) {
            return std::to_string(value.integer);
          }
          if (value.kind == CompileLiteralKind::String) {
            return "'" + value.text + "'";
          }
          if (value.kind == CompileLiteralKind::Null) return "NULL";
          return value.text;
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          return "(" + CompileExprText(value.left) + " " + value.op + " " +
                 CompileExprText(value.right) + ")";
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          return "(" + value.op + " " + CompileExprText(value.operand) + ")";
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          return "(" + CompileExprText(value.operand) +
                 (value.negated ? " is not null)" : " is null)");
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          return "(" + CompileExprText(value.operand) + " " +
                 (value.negated ? "not between " : "between ") +
                 CompileExprText(value.lower) + " and " +
                 CompileExprText(value.upper) + ")";
        } else if constexpr (std::is_same_v<Type, CompileInExpr>) {
          std::string result = "(" + CompileExprText(value.operand) + " " +
                               (value.negated ? "not in (" : "in (");
          for (std::size_t i = 0; i < value.options.size(); ++i) {
            if (i != 0) result += ", ";
            result += CompileExprText(value.options[i]);
          }
          return result + "))";
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          if (value.kind == CompileSubqueryKind::Exists) {
            return "(exists (select ...))";
          }
          if (value.kind == CompileSubqueryKind::In) {
            return "(" + CompileExprText(value.operand) +
                   (value.negated ? " not in (select ...))"
                                  : " in (select ...))");
          }
          return "(select ...)";
        } else {
          std::string result = AggregateName(value.kind) + "(";
          result += value.star ? "*" : CompileExprText(value.argument);
          return result + ")";
        }
      },
      expr->data);
}

Position TokenPosition(const Token &token) noexcept {
  return Position{token.start_offset, token.line, token.column};
}

// 手写递归下降解析器。
//
// 游标 index_ 始终指向“下一个尚未消费的 Token”。Lexer 保证序列末尾有
// EndOfFile，因此 Peek() 无需在每次访问前重复做越界判断。
class ParserImpl {
 public:
  explicit ParserImpl(const std::vector<Token> &tokens) : tokens_(tokens) {}

  // 循环解析直到 EOF。每条语句必须由对应 ParseXxx() 消费结束分号，
  // 这样下一条语句才能从合法的 Token 边界开始。
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
  // 返回当前 Token。调用者不能保存该引用到下一次 Match/Expect 之后，
  // 因为 index_ 可能移动。
  const Token &Peek(std::size_t lookahead = 0) const {
    return tokens_[index_ + lookahead];
  }

  // 仅在确认 index_ > 0 后读取上一个已消费 Token，用于取得 Match 的 lexeme。
  const Token &Previous() const { return tokens_[index_ - 1]; }

  // 若当前 Token 类型匹配，则消费一个 Token 并返回 true；否则游标保持不动。
  bool Match(TokenType type) {
    if (Peek().type != type) return false;
    ++index_;
    return true;
  }

  // 统一构造语法错误，包含期望内容、实际 Token 和位置。实际 lexeme 会附加
  // 在类型名后，例如 AND('and')，便于用户定位。
  Status Error(std::string expected, const Token &actual) const {
    std::string actual_text = TokenTypeName(actual.type);
    if (!actual.lexeme.empty()) actual_text += "('" + actual.lexeme + "')";
    return Status::InvalidArgument("语法错误：期望 " + std::move(expected) + "，实际 " + actual_text +
                                   "，位置 " + std::to_string(actual.line) + ":" +
                                   std::to_string(actual.column));
  }

  // 强制消费指定类型的 Token。所有固定关键字和标点都应通过该函数读取。
  Result<Token> Expect(TokenType type, std::string expected) {
    if (Peek().type != type) return Result<Token>(Error(std::move(expected), Peek()));
    return Result<Token>(tokens_[index_++]);
  }

  // 标识符在 Lexer 中已经转为小写，因此这里直接返回 lexeme 作为规范化名称。
  Result<std::string> ExpectIdentifier(std::string expected) {
    auto token = Expect(TokenType::Identifier, std::move(expected));
    if (!token.ok()) return Result<std::string>(token.status());
    return Result<std::string>(std::move(token.value().lexeme));
  }

  // INSERT 值只允许整数或字符串字面量；布尔值、列引用和表达式不属于当前
  // INSERT 语法。整数使用 from_chars 检查完整消费和 int64 溢出。
  Result<Value> ParseLiteral() {
    if (Match(TokenType::Null)) return Result<Value>(Value());
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
    return Result<Value>(Error("整数、单引号字符串或 NULL", Peek()));
  }

  // 构造 CompileExpr 节点的小型工厂。表达式树使用 shared_ptr<const>，
  // 构造完成后不会在 Parser 内继续修改节点内容。
  CompileExprPtr MakeColumnExpr(std::string table_name, std::string name) {
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(
            CompileColumnExpr{std::move(table_name), std::move(name)}));
  }

  Result<CompileExprPtr> ParseColumnRef(std::string expected) {
    auto first = ExpectIdentifier(std::move(expected));
    if (!first.ok()) return Result<CompileExprPtr>(first.status());
    if (!Match(TokenType::Dot)) {
      return Result<CompileExprPtr>(MakeColumnExpr("", std::move(first.value())));
    }
    auto column = ExpectIdentifier("'.' 后的列名");
    if (!column.ok()) return Result<CompileExprPtr>(column.status());
    return Result<CompileExprPtr>(
        MakeColumnExpr(std::move(first.value()), std::move(column.value())));
  }

  CompileExprPtr MakeLiteralExpr(const Value &value) {
    CompileLiteralExpr literal;
    if (value.IsNull()) {
      literal.kind = CompileLiteralKind::Null;
    } else if (value.IsInt()) {
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

  CompileExprPtr MakeBetween(bool negated, CompileExprPtr operand,
                             CompileExprPtr lower, CompileExprPtr upper) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileBetweenExpr{std::move(operand), std::move(lower),
                           std::move(upper), negated}));
  }

  CompileExprPtr MakeIn(bool negated, CompileExprPtr operand,
                        std::vector<CompileExprPtr> options) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileInExpr{std::move(operand), std::move(options), negated}));
  }

  CompileExprPtr MakeIsNull(CompileExprPtr operand, bool negated) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileIsNullExpr{std::move(operand), negated}));
  }

  CompileExprPtr MakeSubquery(
      CompileSubqueryKind kind,
      std::shared_ptr<const SelectStatement> query,
      CompileExprPtr operand = nullptr, bool negated = false) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileSubqueryExpr{kind, std::move(query), std::move(operand),
                            negated}));
  }

  CompileExprPtr MakeAggregate(CompileAggregateKind kind, bool star,
                               CompileExprPtr argument) {
    return std::make_shared<CompileExpr>(CompileExpr::Data(
        CompileAggregateExpr{kind, star, std::move(argument)}));
  }

  bool IsAggregateCall() const {
    return Peek().type == TokenType::Identifier &&
           Peek(1).type == TokenType::LeftParen &&
           AggregateKindFromName(Peek().lexeme).has_value();
  }

  Result<CompileExprPtr> ParseAggregateExpr() {
    auto function = ExpectIdentifier("聚合函数名");
    if (!function.ok()) return Result<CompileExprPtr>(function.status());
    const auto kind = AggregateKindFromName(function.value());
    if (!kind.has_value()) {
      return Result<CompileExprPtr>(Status::InvalidArgument(
          "未知聚合函数: " + function.value() + "，位置 " +
          std::to_string(Previous().line) + ":" +
          std::to_string(Previous().column)));
    }
    auto left = Expect(TokenType::LeftParen, "聚合函数后的 '('");
    if (!left.ok()) return Result<CompileExprPtr>(left.status());

    bool star = Match(TokenType::Star);
    CompileExprPtr argument;
    if (!star) {
      auto expression = ParseCompileExpr();
      if (!expression.ok()) return expression;
      argument = std::move(expression.value());
    }
    auto right = Expect(TokenType::RightParen, "聚合函数后的 ')'");
    if (!right.ok()) return Result<CompileExprPtr>(right.status());
    return Result<CompileExprPtr>(
        MakeAggregate(*kind, star, std::move(argument)));
  }

  Result<CompileExprPtr> ParseSelectItem() {
    if (IsAggregateCall()) return ParseAggregateExpr();
    if (Peek().type == TokenType::LeftParen ||
        Peek().type == TokenType::Exists) {
      return ParseCompileExpr();
    }
    return ParseColumnRef("SELECT 后的列名或聚合函数");
  }

  // 子查询复用完整 SELECT 递归下降逻辑；能否执行由 Planner 决定。调用前
  // 当前 Token 必须是 SELECT，解析后停在右括号或外层子句起始处。
  Result<std::shared_ptr<const SelectStatement>> ParseSubquery() {
    if (Peek().type != TokenType::Select) {
      return Result<std::shared_ptr<const SelectStatement>>(
          Error("SELECT", Peek()));
    }
    auto parsed = ParseSelect(false);
    if (!parsed.ok()) {
      return Result<std::shared_ptr<const SelectStatement>>(parsed.status());
    }
    auto *select = std::get_if<SelectStatement>(&parsed.value());
    if (select == nullptr) {
      return Result<std::shared_ptr<const SelectStatement>>(
          Status::InternalError("子查询解析结果不是 SELECT"));
    }
    return Result<std::shared_ptr<const SelectStatement>>(
        std::make_shared<SelectStatement>(std::move(*select)));
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

  // 表达式解析采用“每层只处理一种优先级”的递归下降结构：
  // OR -> AND -> NOT -> 比较 -> 加减 -> 乘除 -> 一元 -> 原子。
  // 这样不需要在单个函数中维护运算符优先级表。
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
    // 比较层同时处理普通比较符、BETWEEN、IN 和 LIKE。普通比较只解析一次，
    // 不形成类似 a < b < c 的链式比较；BETWEEN/IN/LIKE 前可带 NOT。
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
    if (Match(TokenType::Is)) {
      const bool negated = Match(TokenType::Not);
      auto null_token = Expect(TokenType::Null, "IS 后的 NULL");
      if (!null_token.ok()) {
        return Result<CompileExprPtr>(null_token.status());
      }
      return Result<CompileExprPtr>(
          MakeIsNull(left.value(), negated));
    }

    bool negated = false;
    if (Peek().type == TokenType::Not &&
        (Peek(1).type == TokenType::Between ||
         Peek(1).type == TokenType::In ||
         Peek(1).type == TokenType::Like)) {
      ++index_;
      negated = true;
    }

    if (Match(TokenType::Between)) {
      auto lower = ParseCompileAdditive();
      if (!lower.ok()) return lower;
      auto and_token = Expect(TokenType::And, "BETWEEN 后的 AND");
      if (!and_token.ok()) return Result<CompileExprPtr>(and_token.status());
      auto upper = ParseCompileAdditive();
      if (!upper.ok()) return upper;
      return Result<CompileExprPtr>(MakeBetween(
          negated, left.value(), lower.value(), upper.value()));
    }

    if (Match(TokenType::In)) {
      auto left_paren = Expect(TokenType::LeftParen, "IN 后的 '('");
      if (!left_paren.ok()) return Result<CompileExprPtr>(left_paren.status());
      if (Peek().type == TokenType::Select) {
        auto subquery = ParseSubquery();
        if (!subquery.ok()) return Result<CompileExprPtr>(subquery.status());
        auto right_paren =
            Expect(TokenType::RightParen, "IN 子查询后的 ')'");
        if (!right_paren.ok()) {
          return Result<CompileExprPtr>(right_paren.status());
        }
        return Result<CompileExprPtr>(MakeSubquery(
            CompileSubqueryKind::In, subquery.value(), left.value(),
            negated));
      }
      std::vector<CompileExprPtr> options;
      while (true) {
        auto option = ParseCompileExpr();
        if (!option.ok()) return option;
        options.push_back(std::move(option.value()));
        if (!Match(TokenType::Comma)) break;
      }
      auto right_paren = Expect(TokenType::RightParen, "IN 列表后的 ')'");
      if (!right_paren.ok()) {
        return Result<CompileExprPtr>(right_paren.status());
      }
      return Result<CompileExprPtr>(
          MakeIn(negated, left.value(), std::move(options)));
    }

    if (Match(TokenType::Like)) {
      auto right = ParseCompileAdditive();
      if (!right.ok()) return right;
      return Result<CompileExprPtr>(MakeBinary(
          negated ? "not like" : "like", left.value(), right.value()));
    }
    return left;
  }

  Result<CompileExprPtr> ParseCompileAdditive() {
    // 左侧结合：a - b - c 解析为 (a - b) - c。
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
    // 乘除优先级高于加减，左侧结合；* 同时也可作为 SELECT 列表中的全列标记，
    // 两个位置由不同解析入口区分。
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
    // 一元负号允许递归构造，例如 -(-x) 会形成嵌套 UnaryExpr。
    if (Match(TokenType::Minus)) {
      auto operand = ParseCompileUnary();
      if (!operand.ok()) return operand;
      return Result<CompileExprPtr>(MakeUnary("-", operand.value()));
    }
    return ParseCompilePrimary();
  }

  Result<CompileExprPtr> ParseCompilePrimary() {
    // 原子表达式包括布尔值、字符串、整数、列名和括号表达式。
    if (Match(TokenType::Null)) {
      return Result<CompileExprPtr>(MakeLiteralExpr(Value()));
    }
    if (Match(TokenType::Exists)) {
      auto left = Expect(TokenType::LeftParen, "EXISTS 后的 '('");
      if (!left.ok()) return Result<CompileExprPtr>(left.status());
      if (Peek().type != TokenType::Select) {
        return Result<CompileExprPtr>(
            Error("SELECT after EXISTS (", Peek()));
      }
      auto subquery = ParseSubquery();
      if (!subquery.ok()) return Result<CompileExprPtr>(subquery.status());
      auto right = Expect(TokenType::RightParen, "EXISTS 子查询后的 ')'");
      if (!right.ok()) return Result<CompileExprPtr>(right.status());
      return Result<CompileExprPtr>(MakeSubquery(
          CompileSubqueryKind::Exists, subquery.value()));
    }
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
      if (IsAggregateCall()) return ParseAggregateExpr();
      return ParseColumnRef("表达式中的列名");
    }
    if (Match(TokenType::LeftParen)) {
      if (Peek().type == TokenType::Select) {
        auto subquery = ParseSubquery();
        if (!subquery.ok()) return Result<CompileExprPtr>(subquery.status());
        auto right = Expect(TokenType::RightParen, "标量子查询后的 ')'");
        if (!right.ok()) return Result<CompileExprPtr>(right.status());
        return Result<CompileExprPtr>(
            MakeSubquery(CompileSubqueryKind::Scalar, subquery.value()));
      }
      auto inner = ParseCompileExpr();
      if (!inner.ok()) return inner;
      auto right = Expect(TokenType::RightParen, "')'");
      if (!right.ok()) return Result<CompileExprPtr>(right.status());
      return inner;
    }
    return Result<CompileExprPtr>(
        Error("表达式（列名、常量、NULL、括号、EXISTS 或 NOT）", Peek()));
  }

  std::optional<Predicate> ExtractPredicate(const CompileExprPtr &expr) {
    return ExtractPredicateFromCompileExpr(expr);
  }

  std::optional<Value> ExtractLiteral(const CompileExprPtr &expr) {
    return ExtractLiteralValue(expr);
  }

  std::optional<JoinCondition> ExtractJoinCondition(
      const CompileExprPtr &expr) const {
    if (expr == nullptr) return std::nullopt;
    const auto *binary = std::get_if<CompileBinaryExpr>(&expr->data);
    if (binary == nullptr || binary->op != "=") return std::nullopt;
    const auto *left =
        binary->left ? std::get_if<CompileColumnExpr>(&binary->left->data)
                     : nullptr;
    const auto *right =
        binary->right ? std::get_if<CompileColumnExpr>(&binary->right->data)
                      : nullptr;
    if (left == nullptr || right == nullptr) return std::nullopt;
    return JoinCondition{left->table_name, left->name, right->table_name,
                         right->name};
  }

  struct ParsedWhere {
    CompileExprPtr expression;
    std::optional<Predicate> predicate;
  };

  Result<ParsedWhere> ParseWhere() {
    // 先保留完整表达式并执行常量折叠，再尝试抽取数据库层可执行的简单等值
    // Predicate。即使无法抽取，compile_where 仍会留给 Planner 做语义检查。
    auto expression = ParseCompileExpr();
    if (!expression.ok()) return Result<ParsedWhere>(expression.status());
    auto folded = FoldCompileExpr(expression.value());
    return Result<ParsedWhere>(
        ParsedWhere{folded, ExtractPredicate(folded)});
  }

  Result<Statement> ParseCreate() {
    // CREATE 分支根据第二个关键字区分 TABLE 和 [UNIQUE] INDEX。
    if (Peek(1).type == TokenType::Table) return ParseCreateTable();
    if (Peek(1).type == TokenType::Index ||
        (Peek(1).type == TokenType::Unique &&
         Peek(2).type == TokenType::Index)) {
      return ParseCreateIndex();
    }
    return Result<Statement>(Error("TABLE 或 INDEX after CREATE", Peek(1)));
  }

  // 解析“列名 类型[(长度)]”。CREATE TABLE 和 ALTER TABLE ADD COLUMN 复用
  // 同一实现，确保两种入口对 VARCHAR 长度和类型名的处理一致。
  Result<Column> ParseColumnDefinition() {
    auto column_name = ExpectIdentifier("列名");
    if (!column_name.ok()) return Result<Column>(column_name.status());
    DataType type;
    std::optional<std::size_t> length;
    if (Match(TokenType::Int)) {
      type = DataType::Int;
    } else if (Match(TokenType::Varchar)) {
      type = DataType::Varchar;
      if (Match(TokenType::LeftParen)) {
        auto length_token = Expect(TokenType::Integer, "VARCHAR 的长度");
        if (!length_token.ok()) return Result<Column>(length_token.status());
        std::size_t parsed_length = 0;
        const auto &text = length_token.value().lexeme;
        const auto parsed =
            std::from_chars(text.data(), text.data() + text.size(),
                            parsed_length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() ||
            parsed_length == 0) {
          return Result<Column>(Status::InvalidArgument(
              "VARCHAR 长度非法，位置 " +
              std::to_string(length_token.value().line) + ":" +
              std::to_string(length_token.value().column)));
        }
        length = parsed_length;
        auto right_length = Expect(TokenType::RightParen, "')'");
        if (!right_length.ok()) {
          return Result<Column>(right_length.status());
        }
      }
    } else {
      return Result<Column>(Error("INT 或 VARCHAR", Peek()));
    }

    Column column(std::move(column_name.value()), type, length);
    bool has_primary_key = false;
    bool has_unique = false;
    bool has_not_null = false;
    bool has_default = false;
    while (true) {
      if (Match(TokenType::Primary)) {
        if (has_primary_key) {
          return Result<Column>(Status::AlreadyExists(
              "列 " + column.name + " 重复声明 PRIMARY KEY"));
        }
        auto key = Expect(TokenType::Key, "PRIMARY 后的 KEY");
        if (!key.ok()) return Result<Column>(key.status());
        has_primary_key = true;
        has_not_null = true;
        has_unique = true;
      } else if (Match(TokenType::Not)) {
        auto null_token = Expect(TokenType::Null, "NOT 后的 NULL");
        if (!null_token.ok()) return Result<Column>(null_token.status());
        if (has_not_null) {
          return Result<Column>(Status::AlreadyExists(
              "列 " + column.name + " 重复声明 NOT NULL"));
        }
        has_not_null = true;
      } else if (Match(TokenType::Unique)) {
        if (has_unique) {
          return Result<Column>(Status::AlreadyExists(
              "列 " + column.name + " 重复声明 UNIQUE"));
        }
        has_unique = true;
      } else if (Match(TokenType::Default)) {
        if (has_default) {
          return Result<Column>(Status::AlreadyExists(
              "列 " + column.name + " 重复声明 DEFAULT"));
        }
        auto default_value = ParseLiteral();
        if (!default_value.ok()) {
          return Result<Column>(default_value.status());
        }
        column.default_value = std::move(default_value.value());
        has_default = true;
      } else {
        break;
      }
    }
    column.nullable = !has_not_null;
    column.primary_key = has_primary_key;
    column.unique = has_unique || has_primary_key;
    return Result<Column>(std::move(column));
  }

  Result<Statement> ParseCreateTable() {
    // 语法：CREATE TABLE name (column type [, ...]);
    // VARCHAR 的长度可选；长度必须为正整数，真正的 Schema 合法性交给 Planner。
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
      auto column = ParseColumnDefinition();
      if (!column.ok()) return Result<Statement>(column.status());
      columns.push_back(std::move(column.value()));
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

  Result<Statement> ParseCreateIndex() {
    // 语法：
    //   CREATE [UNIQUE] INDEX name ON table(column);
    auto create = Expect(TokenType::Create, "CREATE");
    if (!create.ok()) return Result<Statement>(create.status());
    bool is_unique = Match(TokenType::Unique);
    auto index = Expect(TokenType::Index, "INDEX");
    if (!index.ok()) return Result<Statement>(index.status());
    auto index_name = ExpectIdentifier("索引名");
    if (!index_name.ok()) return Result<Statement>(index_name.status());
    auto on = Expect(TokenType::On, "ON");
    if (!on.ok()) return Result<Statement>(on.status());
    auto table_name = ExpectIdentifier("ON 后的表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    auto left = Expect(TokenType::LeftParen, "'('");
    if (!left.ok()) return Result<Statement>(left.status());
    auto column_name = ExpectIdentifier("索引列名");
    if (!column_name.ok()) return Result<Statement>(column_name.status());
    auto right = Expect(TokenType::RightParen, "')'");
    if (!right.ok()) return Result<Statement>(right.status());
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());

    CreateIndexStatement statement{std::move(index_name.value()),
                                   std::move(table_name.value()),
                                   std::move(column_name.value()),
                                   is_unique};
    statement.location = TokenPosition(create.value());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseInsert() {
    // 语法：
    //   INSERT INTO name [(column [, ...])]
    //   VALUES (literal [, ...]) [, (...)]...;
    // Parser 只负责收集目标列和多行字面量，列数、类型和 VARCHAR 长度由
    // Planner 校验。空目标列和空 VALUES 行都由 Expect 报语法错误。
    auto insert = Expect(TokenType::Insert, "INSERT");
    if (!insert.ok()) return Result<Statement>(insert.status());
    auto into = Expect(TokenType::Into, "INTO");
    if (!into.ok()) return Result<Statement>(into.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());

    std::vector<std::string> columns;
    if (Match(TokenType::LeftParen)) {
      while (true) {
        auto column = ExpectIdentifier("INSERT 目标列名");
        if (!column.ok()) return Result<Statement>(column.status());
        columns.push_back(std::move(column.value()));
        if (!Match(TokenType::Comma)) break;
      }
      auto right_columns = Expect(TokenType::RightParen, "')'");
      if (!right_columns.ok()) return Result<Statement>(right_columns.status());
    }

    auto values = Expect(TokenType::Values, "VALUES");
    if (!values.ok()) return Result<Statement>(values.status());

    std::vector<std::vector<Value>> rows;
    while (true) {
      auto left = Expect(TokenType::LeftParen, "VALUES 后的 '('");
      if (!left.ok()) return Result<Statement>(left.status());
      std::vector<Value> row;
      while (true) {
        auto literal = ParseLiteral();
        if (!literal.ok()) return Result<Statement>(literal.status());
        row.push_back(std::move(literal.value()));
        if (!Match(TokenType::Comma)) break;
      }
      auto right = Expect(TokenType::RightParen, "')'");
      if (!right.ok()) return Result<Statement>(right.status());
      rows.push_back(std::move(row));
      if (!Match(TokenType::Comma)) break;
    }

    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    InsertStatement statement{std::move(table_name.value()),
                              std::move(columns), std::move(rows)};
    statement.location = TokenPosition(insert.value());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseSelect(bool consume_semicolon = true) {
    // 语法顺序固定为 SELECT [DISTINCT] 列表 -> FROM [AS 表别名]
    // -> WHERE? -> GROUP BY? -> HAVING? -> ORDER BY? -> LIMIT?。
    auto select = Expect(TokenType::Select, "SELECT");
    if (!select.ok()) return Result<Statement>(select.status());
    SelectStatement statement;
    if (Match(TokenType::Distinct)) {
      statement.distinct = true;
    }
    if (Match(TokenType::Star)) {
      statement.select_all = true;
    } else {
      auto first = ParseSelectItem();
      if (!first.ok()) return Result<Statement>(first.status());
      statement.projection_expressions.push_back(first.value());
      statement.projection.push_back(CompileExprText(first.value()));
      if (Match(TokenType::As)) {
        auto alias = ExpectIdentifier("AS 后的列别名");
        if (!alias.ok()) return Result<Statement>(alias.status());
        statement.projection_aliases.push_back(std::move(alias.value()));
      } else {
        statement.projection_aliases.emplace_back();
      }
      while (Match(TokenType::Comma)) {
        auto item = ParseSelectItem();
        if (!item.ok()) return Result<Statement>(item.status());
        statement.projection_expressions.push_back(item.value());
        statement.projection.push_back(CompileExprText(item.value()));
        if (Match(TokenType::As)) {
          auto alias = ExpectIdentifier("AS 后的列别名");
          if (!alias.ok()) return Result<Statement>(alias.status());
          statement.projection_aliases.push_back(std::move(alias.value()));
        } else {
          statement.projection_aliases.emplace_back();
        }
      }
    }
    auto from = Expect(TokenType::From, "FROM");
    if (!from.ok()) return Result<Statement>(from.status());
    auto table_name = ExpectIdentifier("表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());
    statement.table_name = std::move(table_name.value());
    if (Match(TokenType::As)) {
      auto alias = ExpectIdentifier("AS 后的表别名");
      if (!alias.ok()) return Result<Statement>(alias.status());
      statement.table_alias = std::move(alias.value());
    }
    // FROM 后允许连续 JOIN。每次循环只解析一个子句，Planner 再按顺序构造
    // 左深连接树；裸 JOIN 与 INNER JOIN 等价。
    while (true) {
      std::optional<JoinType> join_type;
      if (Match(TokenType::Left)) {
        (void)Match(TokenType::Outer);
        auto join = Expect(TokenType::Join, "JOIN after LEFT");
        if (!join.ok()) return Result<Statement>(join.status());
        join_type = JoinType::Left;
      } else if (Match(TokenType::Right)) {
        (void)Match(TokenType::Outer);
        auto join = Expect(TokenType::Join, "JOIN after RIGHT");
        if (!join.ok()) return Result<Statement>(join.status());
        join_type = JoinType::Right;
      } else if (Match(TokenType::Full)) {
        (void)Match(TokenType::Outer);
        auto join = Expect(TokenType::Join, "JOIN after FULL");
        if (!join.ok()) return Result<Statement>(join.status());
        join_type = JoinType::Full;
      } else if (Match(TokenType::Inner)) {
        auto join = Expect(TokenType::Join, "JOIN after INNER");
        if (!join.ok()) return Result<Statement>(join.status());
        join_type = JoinType::Inner;
      } else if (Match(TokenType::Join)) {
        join_type = JoinType::Inner;
      } else {
        break;
      }

      JoinClause clause;
      clause.type = *join_type;
      auto right_table = ExpectIdentifier("JOIN 后的表名");
      if (!right_table.ok()) return Result<Statement>(right_table.status());
      clause.table_name = std::move(right_table.value());
      if (Match(TokenType::As)) {
        auto alias = ExpectIdentifier("JOIN 表别名");
        if (!alias.ok()) return Result<Statement>(alias.status());
        clause.table_alias = std::move(alias.value());
      }
      auto on = Expect(TokenType::On, "JOIN 后的 ON");
      if (!on.ok()) return Result<Statement>(on.status());
      clause.location = TokenPosition(on.value());
      auto condition = ParseCompileExpr();
      if (!condition.ok()) return Result<Statement>(condition.status());
      auto folded = FoldCompileExpr(condition.value());
      auto join_condition = ExtractJoinCondition(folded);
      if (!join_condition.has_value()) {
        return Result<Statement>(Status::InvalidArgument(
            "JOIN ON 目前只支持两列等值连接，位置 " +
            std::to_string(on.value().line) + ":" +
            std::to_string(on.value().column)));
      }
      clause.condition = std::move(*join_condition);
      statement.joins.push_back(std::move(clause));
    }
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
    if (Match(TokenType::Having)) {
      auto expression = ParseCompileExpr();
      if (!expression.ok()) return Result<Statement>(expression.status());
      statement.having = FoldCompileExpr(expression.value());
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
    if (Match(TokenType::Limit)) {
      auto limit_token = Expect(TokenType::Integer, "LIMIT 后的非负整数");
      if (!limit_token.ok()) return Result<Statement>(limit_token.status());
      const auto &limit_text = limit_token.value().lexeme;
      if (!limit_text.empty() && limit_text.front() == '-') {
        return Result<Statement>(Status::InvalidArgument(
            "LIMIT 必须是非负整数，位置 " +
            std::to_string(limit_token.value().line) + ":" +
            std::to_string(limit_token.value().column)));
      }
      std::size_t limit = 0;
      const auto parsed = std::from_chars(
          limit_text.data(), limit_text.data() + limit_text.size(), limit);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != limit_text.data() + limit_text.size()) {
        return Result<Statement>(Status::InvalidArgument(
            "LIMIT 数值超出范围，位置 " +
            std::to_string(limit_token.value().line) + ":" +
            std::to_string(limit_token.value().column)));
      }
      statement.limit = limit;
    }
    if (consume_semicolon) {
      auto semicolon = Expect(TokenType::Semicolon, "';'");
      if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    }
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseDelete() {
    // 当前 DELETE 只支持单表以及可选 WHERE；复杂 WHERE 会通过 compile_where
    // 进入语义检查，但执行层级尚未接入。
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
    // 语法：UPDATE table SET col = expr [, ...] [WHERE expr]。
    // 每个赋值都在解析后立即折叠常量，并保留可选的简单 Value。
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

  Result<Statement> ParseAlter() {
    // 当前支持三种结构：
    //   ALTER TABLE t RENAME TO new_t;
    //   ALTER TABLE t RENAME COLUMN old_c TO new_c;
    //   ALTER TABLE t ADD COLUMN c TYPE [DEFAULT literal];
    auto alter = Expect(TokenType::Alter, "ALTER");
    if (!alter.ok()) return Result<Statement>(alter.status());
    auto table = Expect(TokenType::Table, "TABLE");
    if (!table.ok()) return Result<Statement>(table.status());
    auto table_name = ExpectIdentifier("ALTER TABLE 后的表名");
    if (!table_name.ok()) return Result<Statement>(table_name.status());

    AlterTableStatement statement;
    statement.table_name = std::move(table_name.value());
    statement.location = TokenPosition(alter.value());
    if (Match(TokenType::Rename)) {
      if (Match(TokenType::To)) {
        auto new_name = ExpectIdentifier("RENAME TO 后的新表名");
        if (!new_name.ok()) return Result<Statement>(new_name.status());
        statement.action = AlterTableAction::RenameTable;
        statement.new_name = std::move(new_name.value());
      } else {
        auto column = Expect(TokenType::Column, "RENAME 后的 TO 或 COLUMN");
        if (!column.ok()) return Result<Statement>(column.status());
        auto old_name = ExpectIdentifier("RENAME COLUMN 后的旧列名");
        if (!old_name.ok()) return Result<Statement>(old_name.status());
        auto to = Expect(TokenType::To, "旧列名后的 TO");
        if (!to.ok()) return Result<Statement>(to.status());
        auto new_name = ExpectIdentifier("TO 后的新列名");
        if (!new_name.ok()) return Result<Statement>(new_name.status());
        statement.action = AlterTableAction::RenameColumn;
        statement.column_name = std::move(old_name.value());
        statement.new_name = std::move(new_name.value());
      }
    } else {
      auto add = Expect(TokenType::Add, "RENAME 或 ADD after ALTER TABLE name");
      if (!add.ok()) return Result<Statement>(add.status());
      auto column = Expect(TokenType::Column, "ADD 后的 COLUMN");
      if (!column.ok()) return Result<Statement>(column.status());
      auto definition = ParseColumnDefinition();
      if (!definition.ok()) return Result<Statement>(definition.status());
      statement.action = AlterTableAction::AddColumn;
      statement.column = std::move(definition.value());
      statement.column_name = statement.column.name;
      statement.default_value = statement.column.default_value;
    }
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseTransaction() {
    Token keyword = Peek();
    ++index_;
    TransactionStatement statement;
    if (keyword.type == TokenType::Begin) {
      statement.action = TransactionAction::Begin;
    } else if (keyword.type == TokenType::Commit) {
      statement.action = TransactionAction::Commit;
    } else if (keyword.type == TokenType::Rollback) {
      statement.action = TransactionAction::Rollback;
    } else {
      return Result<Statement>(
          Error("BEGIN, COMMIT or ROLLBACK", keyword));
    }
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    statement.location = TokenPosition(keyword);
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseDrop() {
    // 语法：DROP TABLE name; 或 DROP INDEX name;
    auto drop = Expect(TokenType::Drop, "DROP");
    if (!drop.ok()) return Result<Statement>(drop.status());

    if (Match(TokenType::Table)) {
      auto table_name = ExpectIdentifier("DROP TABLE 后的表名");
      if (!table_name.ok()) return Result<Statement>(table_name.status());
      auto semicolon = Expect(TokenType::Semicolon, "';'");
      if (!semicolon.ok()) return Result<Statement>(semicolon.status());
      DropTableStatement statement{std::move(table_name.value())};
      statement.location = TokenPosition(drop.value());
      return Result<Statement>(std::move(statement));
    }

    if (!Match(TokenType::Index)) {
      return Result<Statement>(Error("TABLE 或 INDEX after DROP", Peek()));
    }
    auto index_name = ExpectIdentifier("DROP INDEX 后的索引名");
    if (!index_name.ok()) return Result<Statement>(index_name.status());
    auto semicolon = Expect(TokenType::Semicolon, "';'");
    if (!semicolon.ok()) return Result<Statement>(semicolon.status());
    DropIndexStatement statement{std::move(index_name.value())};
    statement.location = TokenPosition(drop.value());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseExplain() {
    auto explain = Expect(TokenType::Explain, "EXPLAIN");
    if (!explain.ok()) return Result<Statement>(explain.status());
    if (Peek().type != TokenType::Select) {
      return Result<Statement>(Error("SELECT after EXPLAIN", Peek()));
    }
    auto inner = ParseSelect();
    if (!inner.ok()) return inner;
    ExplainStatement statement;
    statement.select =
        std::get<SelectStatement>(std::move(inner.value()));
    statement.location = TokenPosition(explain.value());
    return Result<Statement>(std::move(statement));
  }

  Result<Statement> ParseStatement() {
    // 分派完全依据首个关键字 Token；不支持的关键字会给出预期的语句集合。
    switch (Peek().type) {
      case TokenType::Create: return ParseCreate();
      case TokenType::Insert: return ParseInsert();
      case TokenType::Select: return ParseSelect();
      case TokenType::Explain: return ParseExplain();
      case TokenType::Delete: return ParseDelete();
      case TokenType::Drop: return ParseDrop();
      case TokenType::Alter: return ParseAlter();
      case TokenType::Begin:
      case TokenType::Commit:
      case TokenType::Rollback:
        return ParseTransaction();
      case TokenType::Update: return ParseUpdate();
      default: return Result<Statement>(
          Error("CREATE、INSERT、SELECT、EXPLAIN、DELETE、DROP 或 UPDATE",
                Peek()));
    }
  }

  const std::vector<Token> &tokens_;
  std::size_t index_{0};
};

}  // namespace

Result<std::vector<Statement>> Parser::Parse(std::string_view sql) const {
  // Parser 的统一入口：先完成词法切分，再构造解析器游标。任何词法错误都会
  // 直接作为 Result 返回，不会进入语法阶段。
  Lexer lexer;
  auto tokens = lexer.Tokenize(sql);
  if (!tokens.ok()) return Result<std::vector<Statement>>(tokens.status());
  return ParserImpl(tokens.value()).Parse();
}

std::string ToString(const Statement &statement) {
  // 稳定打印格式用于测试、调试和说明文档；所有语义信息都来自结构化字段，
  // 文本本身不会被再次解析或交给 Executor。
  return std::visit(
      [](const auto &value) -> std::string {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, CreateTableStatement>) {
          return "CreateTableStatement(table=" + value.table_name + ",schema=" + SchemaText(value.schema) + ")";
        } else if constexpr (std::is_same_v<Type, InsertStatement>) {
          std::string result =
              "InsertStatement(table=" + value.table_name + ",columns=";
          if (value.columns.empty()) {
            result += "*";
          } else {
            result += "[";
            for (std::size_t i = 0; i < value.columns.size(); ++i) {
              if (i != 0) result += ",";
              result += value.columns[i];
            }
            result += "]";
          }
          result += ",rows=[";
          for (std::size_t row_index = 0; row_index < value.rows.size();
               ++row_index) {
            if (row_index != 0) result += ",";
            result += "[";
            for (std::size_t i = 0; i < value.rows[row_index].size(); ++i) {
              if (i != 0) result += ",";
              result += QuoteValue(value.rows[row_index][i]);
            }
            result += "]";
          }
          return result + "])";
        } else if constexpr (std::is_same_v<Type, SelectStatement>) {
          std::string result = "SelectStatement(table=" + value.table_name;
          if (!value.table_alias.empty()) {
            result += ",alias=" + value.table_alias;
          }
          if (!value.joins.empty()) {
            result += ",joins=[";
            for (std::size_t i = 0; i < value.joins.size(); ++i) {
              if (i != 0) result += ",";
              const auto &join = value.joins[i];
              result += JoinTypeName(join.type) + ":" + join.table_name;
              if (!join.table_alias.empty()) {
                result += " AS " + join.table_alias;
              }
              result += " ON " + join.condition.left_table + "." +
                        join.condition.left_column + "=" +
                        join.condition.right_table + "." +
                        join.condition.right_column;
            }
            result += "]";
          }
          result += ",projection=";
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
          if (value.having != nullptr) {
            result += ",having=" + CompileExprText(value.having);
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
          if (value.distinct) {
            result += ",distinct=true";
          }
          if (value.limit.has_value()) {
            result += ",limit=" + std::to_string(*value.limit);
          }
          bool has_alias = false;
          for (const auto &alias : value.projection_aliases) {
            if (!alias.empty()) {
              has_alias = true;
              break;
            }
          }
          if (has_alias) {
            result += ",aliases=[";
            for (std::size_t i = 0; i < value.projection_aliases.size(); ++i) {
              if (i != 0) result += ",";
              result += value.projection_aliases[i];
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
        } else if constexpr (std::is_same_v<Type, DropTableStatement>) {
          return "DropTableStatement(table=" + value.table_name + ")";
        } else if constexpr (std::is_same_v<Type, CreateIndexStatement>) {
          return "CreateIndexStatement(index=" + value.index_name +
                 ",table=" + value.table_name +
                 ",column=" + value.column_name +
                 ",unique=" + (value.is_unique ? "true" : "false") + ")";
        } else if constexpr (std::is_same_v<Type, DropIndexStatement>) {
          return "DropIndexStatement(index=" + value.index_name + ")";
        } else if constexpr (std::is_same_v<Type, ExplainStatement>) {
          return "ExplainStatement(select=" + ToString(value.select) + ")";
        } else if constexpr (std::is_same_v<Type, AlterTableStatement>) {
          std::string action =
              value.action == AlterTableAction::RenameTable
                  ? "rename_table"
                  : value.action == AlterTableAction::RenameColumn
                        ? "rename_column"
                        : "add_column";
          std::string result = "AlterTableStatement(table=" + value.table_name +
                               ",action=" + action;
          if (value.action == AlterTableAction::RenameColumn) {
            result += ",column=" + value.column_name;
          } else if (value.action == AlterTableAction::AddColumn) {
            result += ",column=" + value.column.name + ":" +
                      (value.column.type == DataType::Int ? "INT"
                                                         : "VARCHAR");
            if (value.column.length.has_value()) {
              result += "(" + std::to_string(*value.column.length) + ")";
            }
          }
          if (value.action != AlterTableAction::AddColumn) {
            result += ",new_name=" + value.new_name;
          }
          if (value.default_value.has_value()) {
            result += ",default=" + QuoteValue(*value.default_value);
          }
          return result + ")";
        } else if constexpr (std::is_same_v<Type, TransactionStatement>) {
          const char *action =
              value.action == TransactionAction::Begin
                  ? "begin"
                  : value.action == TransactionAction::Commit ? "commit"
                                                              : "rollback";
          return "TransactionStatement(action=" + std::string(action) + ")";
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
