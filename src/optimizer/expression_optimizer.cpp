#include "oursql/optimizer/expression_optimizer.h"

#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace oursql {
namespace {

CompileExprPtr MakeInt(std::int64_t value) {
  CompileLiteralExpr literal;
  literal.kind = CompileLiteralKind::Int;
  literal.integer = value;
  return std::make_shared<CompileExpr>(
      CompileExpr::Data(std::move(literal)));
}

CompileExprPtr MakeString(std::string value) {
  CompileLiteralExpr literal;
  literal.kind = CompileLiteralKind::String;
  literal.text = std::move(value);
  return std::make_shared<CompileExpr>(
      CompileExpr::Data(std::move(literal)));
}

CompileExprPtr MakeBool(bool value) {
  CompileLiteralExpr literal;
  literal.kind = CompileLiteralKind::Bool;
  literal.text = value ? "true" : "false";
  return std::make_shared<CompileExpr>(
      CompileExpr::Data(std::move(literal)));
}

bool IsInt(const CompileExprPtr &expr) {
  const auto *literal =
      expr == nullptr ? nullptr
                      : std::get_if<CompileLiteralExpr>(&expr->data);
  return literal != nullptr && literal->kind == CompileLiteralKind::Int;
}

bool IsString(const CompileExprPtr &expr) {
  const auto *literal =
      expr == nullptr ? nullptr
                      : std::get_if<CompileLiteralExpr>(&expr->data);
  return literal != nullptr && literal->kind == CompileLiteralKind::String;
}

bool IsBool(const CompileExprPtr &expr) {
  const auto *literal =
      expr == nullptr ? nullptr
                      : std::get_if<CompileLiteralExpr>(&expr->data);
  return literal != nullptr && literal->kind == CompileLiteralKind::Bool;
}

const CompileLiteralExpr &Literal(const CompileExprPtr &expr) {
  return std::get<CompileLiteralExpr>(expr->data);
}

bool BoolValue(const CompileExprPtr &expr) {
  return Literal(expr).text == "true";
}

bool SafeAdd(std::int64_t lhs, std::int64_t rhs, std::int64_t *result) {
  if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
    return false;
  }
  *result = lhs + rhs;
  return true;
}

bool SafeSub(std::int64_t lhs, std::int64_t rhs, std::int64_t *result) {
  if ((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) ||
      (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs)) {
    return false;
  }
  *result = lhs - rhs;
  return true;
}

bool SafeMul(std::int64_t lhs, std::int64_t rhs, std::int64_t *result) {
  if (lhs == 0 || rhs == 0) {
    *result = 0;
    return true;
  }
  if (lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  if (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()) {
    return false;
  }
  if (lhs > 0 && rhs > 0 &&
      lhs > std::numeric_limits<std::int64_t>::max() / rhs) {
    return false;
  }
  if (lhs > 0 && rhs < 0 &&
      rhs < std::numeric_limits<std::int64_t>::min() / lhs) {
    return false;
  }
  if (lhs < 0 && rhs > 0 &&
      lhs < std::numeric_limits<std::int64_t>::min() / rhs) {
    return false;
  }
  if (lhs < 0 && rhs < 0 &&
      lhs < std::numeric_limits<std::int64_t>::max() / rhs) {
    return false;
  }
  *result = lhs * rhs;
  return true;
}

int CompareLiterals(const CompileExprPtr &left, const CompileExprPtr &right) {
  if (IsInt(left) && IsInt(right)) {
    const auto lhs = Literal(left).integer;
    const auto rhs = Literal(right).integer;
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
  }
  if (IsString(left) && IsString(right)) {
    const auto &lhs = Literal(left).text;
    const auto &rhs = Literal(right).text;
    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
  }
  const bool lhs = BoolValue(left);
  const bool rhs = BoolValue(right);
  return lhs == rhs ? 0 : (lhs ? 1 : -1);
}

}  // namespace

CompileExprPtr FoldCompileExpr(const CompileExprPtr &expr) {
  if (expr == nullptr) return nullptr;

  if (const auto *binary = std::get_if<CompileBinaryExpr>(&expr->data)) {
    const CompileExprPtr left = FoldCompileExpr(binary->left);
    const CompileExprPtr right = FoldCompileExpr(binary->right);

    if (IsInt(left) && IsInt(right)) {
      const auto lhs = Literal(left).integer;
      const auto rhs = Literal(right).integer;
      std::int64_t result = 0;
      if (binary->op == "+" && SafeAdd(lhs, rhs, &result)) {
        return MakeInt(result);
      }
      if (binary->op == "-" && SafeSub(lhs, rhs, &result)) {
        return MakeInt(result);
      }
      if (binary->op == "*" && SafeMul(lhs, rhs, &result)) {
        return MakeInt(result);
      }
      if (binary->op == "/" && rhs != 0 &&
          !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1)) {
        return MakeInt(lhs / rhs);
      }
    }

    if ((IsBool(left) && IsBool(right)) &&
        (binary->op == "and" || binary->op == "or")) {
      const bool lhs = BoolValue(left);
      const bool rhs = BoolValue(right);
      return MakeBool(binary->op == "and" ? (lhs && rhs) : (lhs || rhs));
    }

    if ((IsInt(left) && IsInt(right)) ||
        (IsString(left) && IsString(right)) ||
        (IsBool(left) && IsBool(right))) {
      const int compare = CompareLiterals(left, right);
      if (binary->op == "=") return MakeBool(compare == 0);
      if (binary->op == "!=" || binary->op == "<>") {
        return MakeBool(compare != 0);
      }
      if (binary->op == ">") return MakeBool(compare > 0);
      if (binary->op == ">=") return MakeBool(compare >= 0);
      if (binary->op == "<") return MakeBool(compare < 0);
      if (binary->op == "<=") return MakeBool(compare <= 0);
    }

    return std::make_shared<CompileExpr>(
        CompileExpr::Data(CompileBinaryExpr{binary->op, left, right}));
  }

  if (const auto *unary = std::get_if<CompileUnaryExpr>(&expr->data)) {
    const CompileExprPtr operand = FoldCompileExpr(unary->operand);
    if (unary->op == "-" && IsInt(operand) &&
        Literal(operand).integer !=
            std::numeric_limits<std::int64_t>::min()) {
      return MakeInt(-Literal(operand).integer);
    }
    if (unary->op == "not" && IsBool(operand)) {
      return MakeBool(!BoolValue(operand));
    }
    return std::make_shared<CompileExpr>(
        CompileExpr::Data(CompileUnaryExpr{unary->op, operand}));
  }

  return expr;
}

std::optional<Predicate> ExtractPredicateFromCompileExpr(
    const CompileExprPtr &expr) {
  if (expr == nullptr) return std::nullopt;
  const auto *binary = std::get_if<CompileBinaryExpr>(&expr->data);
  if (binary == nullptr || binary->op != "=") return std::nullopt;

  const auto *left_column =
      binary->left ? std::get_if<CompileColumnExpr>(&binary->left->data)
                   : nullptr;
  const auto *right_column =
      binary->right ? std::get_if<CompileColumnExpr>(&binary->right->data)
                    : nullptr;
  const auto right_value = ExtractLiteralValue(binary->right);
  const auto left_value = ExtractLiteralValue(binary->left);

  if (left_column != nullptr && right_value.has_value()) {
    return Predicate(left_column->name, *right_value);
  }
  if (right_column != nullptr && left_value.has_value()) {
    return Predicate(right_column->name, *left_value);
  }
  return std::nullopt;
}

std::optional<Value> ExtractLiteralValue(const CompileExprPtr &expr) {
  if (expr == nullptr) return std::nullopt;
  const auto *literal = std::get_if<CompileLiteralExpr>(&expr->data);
  if (literal == nullptr) return std::nullopt;
  if (literal->kind == CompileLiteralKind::Int) {
    return Value(literal->integer);
  }
  if (literal->kind == CompileLiteralKind::String) {
    return Value(literal->text);
  }
  return std::nullopt;
}

}  // namespace oursql
