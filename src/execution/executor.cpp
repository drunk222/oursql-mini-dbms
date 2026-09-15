#include "oursql/execution/executor.h"

#include "oursql/index/index_manager.h"
#include "oursql/planner/planner.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace oursql {

namespace {

// 本文件是数据库功能层的核心：Plan 在这里被映射为表操作。
// 推荐断点顺序：ExecutionEngine::Execute -> 对应 XxxExecutor::Execute
// -> HeapTable::{InsertRow, BeginScan, DeleteRow}。不要先进入 BufferPoolManager。

Status Contextualize(const char *layer, const Status &status) {
  // 保留原错误码，只为消息增加执行层上下文，便于定位失败发生在哪个算子。
  const auto message = std::string(layer) + ": " + status.message();
  switch (status.code()) {
    case ErrorCode::InvalidArgument: return Status::InvalidArgument(message);
    case ErrorCode::NotFound: return Status::NotFound(message);
    case ErrorCode::NotImplemented: return Status::NotImplemented(message);
    case ErrorCode::AlreadyExists: return Status::AlreadyExists(message);
    case ErrorCode::TypeMismatch: return Status::TypeMismatch(message);
    case ErrorCode::OutOfSpace: return Status::OutOfSpace(message);
    case ErrorCode::RecordTooLarge: return Status::RecordTooLarge(message);
    case ErrorCode::IOError: return Status::IOError(message);
    case ErrorCode::InternalError: return Status::InternalError(message);
    case ErrorCode::Ok: return Status::InternalError(message);
  }
  return Status::InternalError(message);
}

class HeapTableSource final : public RowSource {
 public:
  explicit HeapTableSource(HeapTable::ScanCursor cursor) noexcept : cursor_(std::move(cursor)) {}

  // SeqScan 的 RowSource 适配器：把物理表游标接入统一的火山模型接口。
  Result<std::optional<RowEntry>> Next() override { return cursor_.Next(); }

 private:
  HeapTable::ScanCursor cursor_;
};

struct EvalValue {
  enum class Kind { Null, Int, String, Bool } kind{Kind::Null};
  std::int64_t integer{0};
  std::string text;
  bool boolean{false};

  static EvalValue FromValue(const Value &value) {
    if (value.IsNull()) return {};
    if (value.IsInt()) return EvalValue{Kind::Int, value.AsInt(), {}, false};
    return EvalValue{Kind::String, 0, value.AsVarchar(), false};
  }
  static EvalValue Bool(bool value) {
    return EvalValue{Kind::Bool, 0, {}, value};
  }
  [[nodiscard]] bool IsNull() const noexcept { return kind == Kind::Null; }
};

using SubqueryRunner =
    std::function<Result<ExecutionResult>(const SelectStatement &)>;

bool LikeMatches(std::string_view value, std::string_view pattern) {
  std::vector<bool> previous(pattern.size() + 1, false);
  std::vector<bool> current(pattern.size() + 1, false);
  previous[0] = true;
  for (std::size_t j = 1; j <= pattern.size(); ++j) {
    previous[j] = previous[j - 1] && pattern[j - 1] == '%';
  }
  for (std::size_t i = 1; i <= value.size(); ++i) {
    current.assign(pattern.size() + 1, false);
    for (std::size_t j = 1; j <= pattern.size(); ++j) {
      if (pattern[j - 1] == '%') {
        current[j] = current[j - 1] || previous[j];
      } else if (pattern[j - 1] == '_' || pattern[j - 1] == value[i - 1]) {
        current[j] = previous[j - 1];
      }
    }
    previous.swap(current);
  }
  return previous[pattern.size()];
}

Result<EvalValue> EvaluateExpression(const CompileExprPtr &expression,
                                     const Row &row, const Schema &schema,
                                     const std::vector<const Row *> *group = nullptr,
                                     const SubqueryRunner *subquery_runner = nullptr);

Result<int> CompareEval(const EvalValue &left, const EvalValue &right) {
  if (left.kind != right.kind || left.IsNull()) {
    return Result<int>(Status::TypeMismatch("表达式比较的操作数类型不一致"));
  }
  if (left.kind == EvalValue::Kind::Int) {
    return Result<int>(left.integer < right.integer ? -1
                       : left.integer > right.integer ? 1 : 0);
  }
  if (left.kind == EvalValue::Kind::String) {
    return Result<int>(left.text < right.text ? -1
                       : left.text > right.text ? 1 : 0);
  }
  if (left.kind == EvalValue::Kind::Bool) {
    return Result<int>(left.boolean == right.boolean ? 0
                       : left.boolean ? 1 : -1);
  }
  return Result<int>(0);
}

Result<EvalValue> EvaluateExpression(const CompileExprPtr &expression,
                                     const Row &row, const Schema &schema,
                                     const std::vector<const Row *> *group,
                                     const SubqueryRunner *subquery_runner) {
  if (expression == nullptr) {
    return Result<EvalValue>(Status::InvalidArgument("执行表达式为空"));
  }
  return std::visit(
      [&](const auto &node) -> Result<EvalValue> {
        using Type = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          auto index = schema.FindColumnIndex(node.name);
          if (!index.ok()) return Result<EvalValue>(index.status());
          if (index.value() >= row.size()) {
            return Result<EvalValue>(Status::InvalidArgument("表达式输入行列数不足"));
          }
          return Result<EvalValue>(EvalValue::FromValue(row[index.value()]));
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          if (node.kind == CompileLiteralKind::Int) {
            return Result<EvalValue>(EvalValue{EvalValue::Kind::Int, node.integer, {}, false});
          }
          if (node.kind == CompileLiteralKind::String) {
            return Result<EvalValue>(EvalValue{EvalValue::Kind::String, 0, node.text, false});
          }
          if (node.kind == CompileLiteralKind::Bool) {
            return Result<EvalValue>(EvalValue::Bool(node.text == "true"));
          }
          return Result<EvalValue>(EvalValue{});
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          auto operand = EvaluateExpression(node.operand, row, schema, group,
                                            subquery_runner);
          if (!operand.ok() || operand.value().IsNull()) return operand;
          if (node.op == "not" && operand.value().kind == EvalValue::Kind::Bool) {
            return Result<EvalValue>(EvalValue::Bool(!operand.value().boolean));
          }
          if (node.op == "-" && operand.value().kind == EvalValue::Kind::Int) {
            if (operand.value().integer == std::numeric_limits<std::int64_t>::min()) {
              return Result<EvalValue>(Status::InvalidArgument("整数运算溢出"));
            }
            return Result<EvalValue>(EvalValue{EvalValue::Kind::Int,
                                                -operand.value().integer, {}, false});
          }
          return Result<EvalValue>(Status::TypeMismatch("一元运算类型不匹配"));
        } else if constexpr (std::is_same_v<Type, CompileIsNullExpr>) {
          auto operand = EvaluateExpression(node.operand, row, schema, group,
                                            subquery_runner);
          if (!operand.ok()) return operand;
          const bool result = operand.value().IsNull();
          return Result<EvalValue>(EvalValue::Bool(node.negated ? !result : result));
        } else if constexpr (std::is_same_v<Type, CompileBetweenExpr>) {
          auto operand = EvaluateExpression(node.operand, row, schema, group,
                                            subquery_runner);
          auto lower = EvaluateExpression(node.lower, row, schema, group,
                                          subquery_runner);
          auto upper = EvaluateExpression(node.upper, row, schema, group,
                                          subquery_runner);
          if (!operand.ok()) return operand;
          if (!lower.ok()) return lower;
          if (!upper.ok()) return upper;
          if (operand.value().IsNull() || lower.value().IsNull() || upper.value().IsNull()) {
            return Result<EvalValue>(EvalValue{});
          }
          auto low = CompareEval(operand.value(), lower.value());
          auto high = CompareEval(operand.value(), upper.value());
          if (!low.ok()) return Result<EvalValue>(low.status());
          if (!high.ok()) return Result<EvalValue>(high.status());
          bool result = low.value() >= 0 && high.value() <= 0;
          return Result<EvalValue>(EvalValue::Bool(node.negated ? !result : result));
        } else if constexpr (std::is_same_v<Type, CompileInExpr>) {
          auto operand = EvaluateExpression(node.operand, row, schema, group,
                                            subquery_runner);
          if (!operand.ok()) return operand;
          if (operand.value().IsNull()) return Result<EvalValue>(EvalValue{});
          bool saw_null = false;
          for (const auto &option_expr : node.options) {
            auto option = EvaluateExpression(option_expr, row, schema, group,
                                             subquery_runner);
            if (!option.ok()) return option;
            if (option.value().IsNull()) { saw_null = true; continue; }
            auto comparison = CompareEval(operand.value(), option.value());
            if (!comparison.ok()) return Result<EvalValue>(comparison.status());
            if (comparison.value() == 0) {
              return Result<EvalValue>(EvalValue::Bool(!node.negated));
            }
          }
          if (saw_null) return Result<EvalValue>(EvalValue{});
          return Result<EvalValue>(EvalValue::Bool(node.negated));
        } else if constexpr (std::is_same_v<Type, CompileAggregateExpr>) {
          if (group == nullptr) {
            return Result<EvalValue>(Status::InvalidArgument("聚合表达式缺少分组上下文"));
          }
          if (node.kind == CompileAggregateKind::Count) {
            std::int64_t count = 0;
            if (node.star) count = static_cast<std::int64_t>(group->size());
            else for (const Row *item : *group) {
              auto value = EvaluateExpression(node.argument, *item, schema,
                                              nullptr, subquery_runner);
              if (!value.ok()) return value;
              if (!value.value().IsNull()) ++count;
            }
            return Result<EvalValue>(EvalValue{EvalValue::Kind::Int, count, {}, false});
          }
          bool found = false;
          EvalValue accumulated;
          std::int64_t count = 0;
          for (const Row *item : *group) {
            auto value = EvaluateExpression(node.argument, *item, schema,
                                            nullptr, subquery_runner);
            if (!value.ok()) return value;
            if (value.value().IsNull()) continue;
            if (!found) { accumulated = value.value(); found = true; }
            else if (node.kind == CompileAggregateKind::Sum ||
                     node.kind == CompileAggregateKind::Avg) {
              const auto rhs = value.value().integer;
              const auto lhs = accumulated.integer;
              if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
                  (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
                return Result<EvalValue>(Status::InvalidArgument("聚合整数溢出"));
              }
              accumulated.integer += rhs;
            } else {
              auto comparison = CompareEval(value.value(), accumulated);
              if (!comparison.ok()) return Result<EvalValue>(comparison.status());
              if ((node.kind == CompileAggregateKind::Min && comparison.value() < 0) ||
                  (node.kind == CompileAggregateKind::Max && comparison.value() > 0)) {
                accumulated = value.value();
              }
            }
            ++count;
          }
          if (!found) return Result<EvalValue>(EvalValue{});
          if (node.kind == CompileAggregateKind::Avg) accumulated.integer /= count;
          return Result<EvalValue>(std::move(accumulated));
        } else if constexpr (std::is_same_v<Type, CompileSubqueryExpr>) {
          if (subquery_runner == nullptr || node.query == nullptr) {
            return Result<EvalValue>(Status::InvalidArgument(
                "子查询缺少执行上下文"));
          }
          auto result = (*subquery_runner)(*node.query);
          if (!result.ok()) return Result<EvalValue>(result.status());
          if (node.kind == CompileSubqueryKind::Exists) {
            const bool exists = !result.value().rows.empty();
            return Result<EvalValue>(
                EvalValue::Bool(node.negated ? !exists : exists));
          }
          if (result.value().column_names.size() != 1) {
            return Result<EvalValue>(Status::InvalidArgument(
                "子查询必须返回一列"));
          }
          if (node.kind == CompileSubqueryKind::Scalar) {
            if (result.value().rows.size() > 1) {
              return Result<EvalValue>(Status::InvalidArgument(
                  "标量子查询返回了多行"));
            }
            if (result.value().rows.empty()) return Result<EvalValue>(EvalValue{});
            return Result<EvalValue>(
                EvalValue::FromValue(result.value().rows.front().front()));
          }
          auto operand = EvaluateExpression(node.operand, row, schema, group,
                                            subquery_runner);
          if (!operand.ok()) return operand;
          if (operand.value().IsNull()) return Result<EvalValue>(EvalValue{});
          bool saw_null = false;
          for (const auto &result_row : result.value().rows) {
            if (result_row.empty()) {
              return Result<EvalValue>(Status::InvalidArgument(
                  "子查询结果行缺少列"));
            }
            EvalValue option = EvalValue::FromValue(result_row.front());
            if (option.IsNull()) { saw_null = true; continue; }
            auto comparison = CompareEval(operand.value(), option);
            if (!comparison.ok()) return Result<EvalValue>(comparison.status());
            if (comparison.value() == 0) {
              return Result<EvalValue>(EvalValue::Bool(!node.negated));
            }
          }
          if (saw_null) return Result<EvalValue>(EvalValue{});
          return Result<EvalValue>(EvalValue::Bool(node.negated));
        } else {
          auto left = EvaluateExpression(node.left, row, schema, group,
                                         subquery_runner);
          if (!left.ok()) return left;
          if (node.op == "and" && left.value().kind == EvalValue::Kind::Bool &&
              !left.value().boolean) return Result<EvalValue>(EvalValue::Bool(false));
          if (node.op == "or" && left.value().kind == EvalValue::Kind::Bool &&
              left.value().boolean) return Result<EvalValue>(EvalValue::Bool(true));
          auto right = EvaluateExpression(node.right, row, schema, group,
                                          subquery_runner);
          if (!right.ok()) return right;
          if (node.op == "and" || node.op == "or") {
            if (node.op == "and" && right.value().kind == EvalValue::Kind::Bool &&
                !right.value().boolean) return Result<EvalValue>(EvalValue::Bool(false));
            if (node.op == "or" && right.value().kind == EvalValue::Kind::Bool &&
                right.value().boolean) return Result<EvalValue>(EvalValue::Bool(true));
            if (left.value().IsNull() || right.value().IsNull()) {
              return Result<EvalValue>(EvalValue{});
            }
            if (left.value().kind != EvalValue::Kind::Bool || right.value().kind != EvalValue::Kind::Bool) {
              return Result<EvalValue>(Status::TypeMismatch("逻辑运算需要 BOOL"));
            }
            return Result<EvalValue>(EvalValue::Bool(node.op == "and"
                ? left.value().boolean && right.value().boolean
                : left.value().boolean || right.value().boolean));
          }
          if (left.value().IsNull() || right.value().IsNull()) return Result<EvalValue>(EvalValue{});
          if (node.op == "like" || node.op == "not like") {
            bool matches = LikeMatches(left.value().text, right.value().text);
            return Result<EvalValue>(EvalValue::Bool(node.op == "like" ? matches : !matches));
          }
          if (node.op == "+" || node.op == "-" || node.op == "*" || node.op == "/") {
            const auto lhs = left.value().integer;
            const auto rhs = right.value().integer;
            std::int64_t result = 0;
            bool valid = true;
            if (node.op == "+") {
              valid = !((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
                        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs));
              if (valid) result = lhs + rhs;
            } else if (node.op == "-") {
              valid = !((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) ||
                        (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs));
              if (valid) result = lhs - rhs;
            } else if (node.op == "*") {
              if (lhs == 0 || rhs == 0) result = 0;
              else if ((lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) ||
                       (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min())) valid = false;
              else {
                if (lhs > 0 && rhs > 0) valid = lhs <= std::numeric_limits<std::int64_t>::max() / rhs;
                if (lhs > 0 && rhs < 0) valid = rhs >= std::numeric_limits<std::int64_t>::min() / lhs;
                if (lhs < 0 && rhs > 0) valid = lhs >= std::numeric_limits<std::int64_t>::min() / rhs;
                if (lhs < 0 && rhs < 0) valid = lhs >= std::numeric_limits<std::int64_t>::max() / rhs;
                if (valid) result = lhs * rhs;
              }
            } else {
              valid = rhs != 0 && !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1);
              if (valid) result = lhs / rhs;
            }
            if (!valid) return Result<EvalValue>(Status::InvalidArgument(rhs == 0 && node.op == "/" ? "除数不能为零" : "整数运算溢出"));
            return Result<EvalValue>(EvalValue{EvalValue::Kind::Int, result, {}, false});
          }
          auto comparison = CompareEval(left.value(), right.value());
          if (!comparison.ok()) return Result<EvalValue>(comparison.status());
          bool result = node.op == "=" ? comparison.value() == 0
              : (node.op == "!=" || node.op == "<>") ? comparison.value() != 0
              : node.op == ">" ? comparison.value() > 0
              : node.op == ">=" ? comparison.value() >= 0
              : node.op == "<" ? comparison.value() < 0
              : comparison.value() <= 0;
          return Result<EvalValue>(EvalValue::Bool(result));
        }
      }, expression->data);
}

Result<Value> ToStorageValue(const EvalValue &value) {
  if (value.kind == EvalValue::Kind::Null) return Result<Value>(Value{});
  if (value.kind == EvalValue::Kind::Int) return Result<Value>(Value(value.integer));
  if (value.kind == EvalValue::Kind::String) return Result<Value>(Value(value.text));
  return Result<Value>(Status::TypeMismatch("结果集尚不支持 BOOL 物理值"));
}

class FilterSource final : public RowSource {
 public:
  FilterSource(std::unique_ptr<RowSource> child, std::size_t column_index,
               Predicate predicate, CompileExprPtr expression, Schema schema,
               SubqueryRunner subquery_runner)
      : child_(std::move(child)),
        column_index_(column_index),
        predicate_(std::move(predicate)),
        expression_(std::move(expression)),
        schema_(std::move(schema)),
        subquery_runner_(std::move(subquery_runner)) {}

  Result<std::optional<RowEntry>> Next() override {
    // 惰性过滤：每次只向下游索要一行，匹配才返回；因此不会先把整表装入内存。
    while (true) {
      auto input = child_->Next();
      if (!input.ok()) return Result<std::optional<RowEntry>>(input.status());
      if (!input.value().has_value()) return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
      auto entry = std::move(input.value().value());
      if (entry.second.size() != schema_.size()) {
        return Result<std::optional<RowEntry>>(
            Status::InvalidArgument("Filter 输入行列数与 Schema 不符"));
      }
      bool matches = false;
      if (expression_ != nullptr) {
        auto evaluated = EvaluateExpression(
            expression_, entry.second, schema_, nullptr,
            subquery_runner_ ? &subquery_runner_ : nullptr);
        if (!evaluated.ok()) return Result<std::optional<RowEntry>>(evaluated.status());
        matches = evaluated.value().kind == EvalValue::Kind::Bool &&
                  evaluated.value().boolean;
      } else {
        const auto &value = entry.second[column_index_];
        if (predicate_.kind == PredicateKind::IsNull) matches = value.IsNull();
        else if (predicate_.kind == PredicateKind::IsNotNull) matches = !value.IsNull();
        else matches = !value.IsNull() && value.type() == predicate_.value.type() &&
                       value == predicate_.value;
      }
      if (matches) {
        return Result<std::optional<RowEntry>>(
            std::optional<RowEntry>(std::move(entry)));
      }
    }
  }

 private:
  std::unique_ptr<RowSource> child_;
  std::size_t column_index_{0};
  Predicate predicate_;
  CompileExprPtr expression_;
  Schema schema_;
  SubqueryRunner subquery_runner_;
};

class ExpressionProjectSource final : public RowSource {
 public:
  ExpressionProjectSource(std::unique_ptr<RowSource> child,
                          std::vector<CompileExprPtr> expressions,
                          Schema input_schema, SubqueryRunner subquery_runner)
      : child_(std::move(child)), expressions_(std::move(expressions)),
        input_schema_(std::move(input_schema)),
        subquery_runner_(std::move(subquery_runner)) {}

  Result<std::optional<RowEntry>> Next() override {
    auto input = child_->Next();
    if (!input.ok() || !input.value().has_value()) return input;
    auto entry = std::move(input.value().value());
    Row projected;
    projected.reserve(expressions_.size());
    for (const auto &expression : expressions_) {
      auto evaluated = EvaluateExpression(
          expression, entry.second, input_schema_, nullptr,
          subquery_runner_ ? &subquery_runner_ : nullptr);
      if (!evaluated.ok()) {
        return Result<std::optional<RowEntry>>(evaluated.status());
      }
      auto stored = ToStorageValue(evaluated.value());
      if (!stored.ok()) return Result<std::optional<RowEntry>>(stored.status());
      projected.push_back(std::move(stored.value()));
    }
    entry.second = std::move(projected);
    return Result<std::optional<RowEntry>>(
        std::optional<RowEntry>(std::move(entry)));
  }

 private:
  std::unique_ptr<RowSource> child_;
  std::vector<CompileExprPtr> expressions_;
  Schema input_schema_;
  SubqueryRunner subquery_runner_;
};

class ProjectSource final : public RowSource {
 public:
  ProjectSource(std::unique_ptr<RowSource> child, std::vector<std::size_t> indexes,
                Schema schema)
      : child_(std::move(child)), indexes_(std::move(indexes)), schema_(std::move(schema)) {}

  Result<std::optional<RowEntry>> Next() override {
    // 投影只重排/截取 Row 中的列，RID 保留给 DELETE 或未来 UPDATE 定位原记录。
    auto input = child_->Next();
    if (!input.ok()) return Result<std::optional<RowEntry>>(input.status());
    if (!input.value().has_value()) return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
    auto entry = std::move(input.value().value());
    if (entry.second.size() != schema_.size()) {
      return Result<std::optional<RowEntry>>(
          Status::InvalidArgument("Project 输入行列数与 Schema 不符"));
    }
    Row projected;
    projected.reserve(indexes_.size());
    for (const auto index : indexes_) projected.push_back(entry.second[index]);
    entry.second = std::move(projected);
    return Result<std::optional<RowEntry>>(std::optional<RowEntry>(std::move(entry)));
  }

 private:
  std::unique_ptr<RowSource> child_;
  std::vector<std::size_t> indexes_;
  Schema schema_;
};

class MaterializedSource final : public RowSource {
 public:
  explicit MaterializedSource(std::vector<RowEntry> rows) noexcept
      : rows_(std::move(rows)) {}

  Result<std::optional<RowEntry>> Next() override {
    if (next_ == rows_.size()) {
      return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
    }
    return Result<std::optional<RowEntry>>(
        std::optional<RowEntry>(std::move(rows_[next_++])));
  }

 private:
  std::vector<RowEntry> rows_;
  std::size_t next_{0};
};

class LimitSource final : public RowSource {
 public:
  LimitSource(std::unique_ptr<RowSource> child, std::size_t limit)
      : child_(std::move(child)), limit_(limit) {}
  Result<std::optional<RowEntry>> Next() override {
    if (emitted_ >= limit_) {
      return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
    }
    auto next = child_->Next();
    if (next.ok() && next.value().has_value()) ++emitted_;
    return next;
  }
 private:
  std::unique_ptr<RowSource> child_;
  std::size_t limit_{0};
  std::size_t emitted_{0};
};

class DistinctSource final : public RowSource {
 public:
  explicit DistinctSource(std::unique_ptr<RowSource> child)
      : child_(std::move(child)) {}
  Result<std::optional<RowEntry>> Next() override {
    while (true) {
      auto next = child_->Next();
      if (!next.ok() || !next.value().has_value()) return next;
      const Row &candidate = next.value()->second;
      const bool duplicate = std::any_of(
          seen_.begin(), seen_.end(), [&](const Row &row) { return row == candidate; });
      if (!duplicate) {
        seen_.push_back(candidate);
        return next;
      }
    }
  }
 private:
  std::unique_ptr<RowSource> child_;
  std::vector<Row> seen_;
};

Result<std::vector<RowEntry>> Materialize(std::unique_ptr<RowSource> child,
                                         const char *executor_name);

Result<std::unique_ptr<RowSource>> BuildJoinSource(
    std::unique_ptr<RowSource> left, std::unique_ptr<RowSource> right,
    const JoinPlan &plan, std::size_t left_width, std::size_t right_width) {
  auto left_rows = Materialize(std::move(left), "JoinExecutor");
  if (!left_rows.ok()) return Result<std::unique_ptr<RowSource>>(left_rows.status());
  auto right_rows = Materialize(std::move(right), "JoinExecutor");
  if (!right_rows.ok()) return Result<std::unique_ptr<RowSource>>(right_rows.status());
  if (!plan.left_column_index.has_value() || !plan.right_column_index.has_value() ||
      *plan.left_column_index >= left_width || *plan.right_column_index >= right_width) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("JOIN 列下标无效"));
  }
  std::vector<RowEntry> output;
  std::vector<bool> right_matched(right_rows.value().size(), false);
  for (auto &left_entry : left_rows.value()) {
    bool matched = false;
    for (std::size_t r = 0; r < right_rows.value().size(); ++r) {
      const auto &right_entry = right_rows.value()[r];
      const Value &lhs = left_entry.second[*plan.left_column_index];
      const Value &rhs = right_entry.second[*plan.right_column_index];
      if (lhs.IsNull() || rhs.IsNull() || lhs != rhs) continue;
      Row joined = left_entry.second;
      joined.insert(joined.end(), right_entry.second.begin(), right_entry.second.end());
      output.emplace_back(left_entry.first, std::move(joined));
      matched = true;
      right_matched[r] = true;
    }
    if (!matched && (plan.join_type == JoinType::Left || plan.join_type == JoinType::Full)) {
      Row joined = left_entry.second;
      joined.resize(left_width + right_width, Value{});
      output.emplace_back(left_entry.first, std::move(joined));
    }
  }
  if (plan.join_type == JoinType::Right || plan.join_type == JoinType::Full) {
    for (std::size_t r = 0; r < right_rows.value().size(); ++r) {
      if (right_matched[r]) continue;
      Row joined(left_width, Value{});
      joined.insert(joined.end(), right_rows.value()[r].second.begin(),
                    right_rows.value()[r].second.end());
      output.emplace_back(right_rows.value()[r].first, std::move(joined));
    }
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<MaterializedSource>(std::move(output));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::vector<RowEntry>> Materialize(std::unique_ptr<RowSource> child,
                                         const char *executor_name) {
  if (child == nullptr) {
    return Result<std::vector<RowEntry>>(
        Status::InvalidArgument(std::string(executor_name) + " 缺少子算子"));
  }
  std::vector<RowEntry> rows;
  while (true) {
    auto entry = child->Next();
    if (!entry.ok()) return Result<std::vector<RowEntry>>(entry.status());
    if (!entry.value().has_value()) break;
    rows.push_back(std::move(entry.value().value()));
  }
  return Result<std::vector<RowEntry>>(std::move(rows));
}

int CompareValue(const Value &left, const Value &right) {
  if (left.IsNull() || right.IsNull()) {
    return left.IsNull() == right.IsNull() ? 0 : left.IsNull() ? -1 : 1;
  }
  if (left.IsInt()) {
    return left.AsInt() < right.AsInt() ? -1
           : left.AsInt() > right.AsInt() ? 1
                                          : 0;
  }
  return left.AsVarchar() < right.AsVarchar() ? -1
         : left.AsVarchar() > right.AsVarchar() ? 1
                                                : 0;
}

bool SameGroup(const Row &left, const Row &right,
               const std::vector<std::size_t> &indexes) {
  for (const auto index : indexes) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

Result<Value> EvaluateUpdateExpression(const CompileExprPtr &expression,
                                       const Row &row,
                                       const Schema &schema) {
  if (expression == nullptr) {
    return Result<Value>(Status::InvalidArgument("UPDATE SET 表达式为空"));
  }
  return std::visit(
      [&](const auto &node) -> Result<Value> {
        using Type = std::decay_t<decltype(node)>;
        if constexpr (std::is_same_v<Type, CompileColumnExpr>) {
          auto index = schema.FindColumnIndex(node.name);
          if (!index.ok()) return Result<Value>(index.status());
          return Result<Value>(row[index.value()]);
        } else if constexpr (std::is_same_v<Type, CompileLiteralExpr>) {
          if (node.kind == CompileLiteralKind::Int) {
            return Result<Value>(Value(node.integer));
          }
          if (node.kind == CompileLiteralKind::String) {
            return Result<Value>(Value(node.text));
          }
          return Result<Value>(
              Status::TypeMismatch("UPDATE SET 不能产生 BOOL 值"));
        } else if constexpr (std::is_same_v<Type, CompileUnaryExpr>) {
          auto operand = EvaluateUpdateExpression(node.operand, row, schema);
          if (!operand.ok()) return operand;
          if (node.op != "-" || !operand.value().IsInt()) {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 不支持的一元运算符: " + node.op));
          }
          if (operand.value().AsInt() == std::numeric_limits<std::int64_t>::min()) {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 整数运算溢出"));
          }
          return Result<Value>(Value(-operand.value().AsInt()));
        } else if constexpr (std::is_same_v<Type, CompileBinaryExpr>) {
          auto left = EvaluateUpdateExpression(node.left, row, schema);
          if (!left.ok()) return left;
          auto right = EvaluateUpdateExpression(node.right, row, schema);
          if (!right.ok()) return right;
          if (!left.value().IsInt() || !right.value().IsInt()) {
            return Result<Value>(Status::TypeMismatch(
                "UPDATE SET 算术运算需要 INT 操作数"));
          }
          const auto lhs = left.value().AsInt();
          const auto rhs = right.value().AsInt();
          std::int64_t value = 0;
          bool valid = false;
          if (node.op == "+") {
            valid = !((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
                      (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs));
            if (valid) value = lhs + rhs;
          } else if (node.op == "-") {
            valid = !((rhs > 0 && lhs < std::numeric_limits<std::int64_t>::min() + rhs) ||
                      (rhs < 0 && lhs > std::numeric_limits<std::int64_t>::max() + rhs));
            if (valid) value = lhs - rhs;
          } else if (node.op == "*") {
            if (lhs == 0 || rhs == 0) {
              valid = true;
              value = 0;
            } else if (!((lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) ||
                         (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()))) {
              if (lhs > 0 && rhs > 0) valid = lhs <= std::numeric_limits<std::int64_t>::max() / rhs;
              if (lhs > 0 && rhs < 0) valid = rhs >= std::numeric_limits<std::int64_t>::min() / lhs;
              if (lhs < 0 && rhs > 0) valid = lhs >= std::numeric_limits<std::int64_t>::min() / rhs;
              if (lhs < 0 && rhs < 0) valid = lhs >= std::numeric_limits<std::int64_t>::max() / rhs;
              if (valid) value = lhs * rhs;
            }
          } else if (node.op == "/") {
            valid = rhs != 0 &&
                    !(lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1);
            if (valid) value = lhs / rhs;
          } else {
            return Result<Value>(Status::InvalidArgument(
                "UPDATE SET 不支持的二元运算符: " + node.op));
          }
          if (!valid) {
            return Result<Value>(Status::InvalidArgument(
                node.op == "/" && rhs == 0 ? "UPDATE SET 除数不能为零"
                                           : "UPDATE SET 整数运算溢出"));
          }
          return Result<Value>(Value(value));
        } else {
          // BETWEEN、IN 和聚合属于 SELECT/HAVING 编译层谓词，UPDATE SET
          // 尚未提供这些值语义，必须显式拒绝。
          return Result<Value>(Status::NotImplemented(
              "UPDATE SET 暂不支持 BETWEEN、IN 或聚合表达式"));
        }
      },
      expression->data);
}

Result<const TableMetadata *> RequireMetadata(Catalog *catalog, const std::string &table_name) {
  if (catalog == nullptr) {
    return Result<const TableMetadata *>(Status::InvalidArgument("执行器缺少 Catalog"));
  }
  auto metadata = catalog->GetTableMetadata(table_name);
  if (!metadata.ok()) return Result<const TableMetadata *>(metadata.status());
  if (metadata.value() == nullptr) {
    return Result<const TableMetadata *>(Status::InternalError("Catalog 返回空表元数据: " + table_name));
  }
  return metadata;
}

}  // namespace

Result<ExecutionResult> CreateTableExecutor::Execute(const CreateTablePlan &plan) const {
  if (catalog_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("CreateTableExecutor 缺少 Catalog"));
  }
  // 首数据页由持久化 Catalog 分配，计划层只提供逻辑表名和 Schema。
  auto status = catalog_->CreateTable(TableMetadata{plan.table_name, plan.schema, INVALID_PAGE_ID});
  if (!status.ok()) return Result<ExecutionResult>(Contextualize("CreateTableExecutor", status));
  return Result<ExecutionResult>(ExecutionResult{});
}

Result<ExecutionResult> InsertExecutor::Execute(const InsertPlan &plan,
                                                Transaction *transaction) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("InsertExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) return Result<ExecutionResult>(Contextualize("InsertExecutor", metadata.status()));
  // Planner 已把指定列、DEFAULT 和可空列全部规范化为 Schema 顺序。
  // 逐行写入并同步维护索引；任一行失败时由上层事务回滚整批修改。
  HeapTable table(buffer_pool_, *metadata.value());
  IndexManager index_manager(catalog_, buffer_pool_);
  ExecutionResult result;
  const auto rollback_direct = [&](std::size_t inserted_count) {
    if (transaction != nullptr) return;
    while (inserted_count > 0) {
      --inserted_count;
      const RID rid = result.rids[inserted_count];
      (void)index_manager.OnDelete(plan.table_name, plan.rows[inserted_count], rid);
      (void)table.DeleteRow(rid);
    }
    result.rids.clear();
    result.affected_rows = 0;
  };
  for (std::size_t row_index = 0; row_index < plan.rows.size(); ++row_index) {
    const auto &row = plan.rows[row_index];
    auto rid = table.InsertRow(row, transaction);
    if (!rid.ok()) {
      if (transaction != nullptr) transaction->MarkFailed();
      else rollback_direct(row_index);
      return Result<ExecutionResult>(Contextualize("InsertExecutor", rid.status()));
    }
    auto index_status = index_manager.OnInsert(plan.table_name, row,
                                               rid.value(), transaction);
    if (!index_status.ok()) {
      if (transaction == nullptr) {
        (void)table.DeleteRow(rid.value());
        rollback_direct(row_index);
      } else {
        transaction->MarkFailed();
      }
      return Result<ExecutionResult>(
          Contextualize("InsertExecutor", index_status));
    }
    result.rids.push_back(rid.value());
    ++result.affected_rows;
  }
  return Result<ExecutionResult>(std::move(result));
}

Result<std::unique_ptr<RowSource>> SeqScanExecutor::Execute(const SeqScanPlan &plan) const {
  if (buffer_pool_ == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("SeqScanExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) {
    return Result<std::unique_ptr<RowSource>>(Contextualize("SeqScanExecutor", metadata.status()));
  }
  HeapTable table(buffer_pool_, *metadata.value());
  auto cursor = table.BeginScan();
  if (!cursor.ok()) {
    return Result<std::unique_ptr<RowSource>>(Contextualize("SeqScanExecutor", cursor.status()));
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<HeapTableSource>(std::move(cursor.value()));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> FilterExecutor::Execute(
    const FilterPlan &plan, std::unique_ptr<RowSource> child,
    const Schema &schema, SubqueryRunner subquery_runner) const {
  if (child == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("FilterExecutor 缺少子算子"));
  }
  std::size_t column_index = 0;
  if (plan.expression == nullptr) {
    auto column = schema.FindColumnIndex(plan.predicate.column);
    if (!column.ok()) return Result<std::unique_ptr<RowSource>>(column.status());
    column_index = column.value();
  }
  if (plan.expression == nullptr && plan.predicate.kind == PredicateKind::Equal &&
      schema.At(column_index).type != plan.predicate.value.type()) {
    return Result<std::unique_ptr<RowSource>>(
        Status::TypeMismatch("Filter 列和值类型不匹配: " + plan.predicate.column));
  }
  std::unique_ptr<RowSource> source = std::make_unique<FilterSource>(
      std::move(child), column_index, plan.predicate, plan.expression, schema,
      std::move(subquery_runner));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> ProjectExecutor::Execute(
    const ProjectPlan &plan, std::unique_ptr<RowSource> child, const Schema &schema) const {
  if (child == nullptr) {
    return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument("ProjectExecutor 缺少子算子"));
  }
  std::vector<std::size_t> indexes;
  // 在构建阶段把列名解析成下标，使逐行 Next() 不再重复查找 Schema。
  if (!plan.input_indexes.empty()) {
    indexes = plan.input_indexes;
    for (const auto index : indexes) {
      if (index >= schema.size()) {
        return Result<std::unique_ptr<RowSource>>(
            Status::InvalidArgument("Project 输入列下标越界"));
      }
    }
  } else if (plan.select_all) {
    indexes.reserve(schema.size());
    for (std::size_t index = 0; index < schema.size(); ++index) indexes.push_back(index);
  } else {
    indexes.reserve(plan.columns.size());
    for (const auto &name : plan.columns) {
      auto index = schema.FindColumnIndex(name);
      if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
      indexes.push_back(index.value());
    }
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<ProjectSource>(std::move(child), std::move(indexes), schema);
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> GroupByExecutor::Execute(
    const GroupByPlan &plan, std::unique_ptr<RowSource> child,
    const Schema &schema) const {
  std::vector<std::size_t> indexes;
  indexes.reserve(plan.columns.size());
  for (const auto &name : plan.columns) {
    auto index = schema.FindColumnIndex(name);
    if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
    indexes.push_back(index.value());
  }
  auto input = Materialize(std::move(child), "GroupByExecutor");
  if (!input.ok()) return Result<std::unique_ptr<RowSource>>(input.status());
  std::vector<RowEntry> groups;
  for (auto &entry : input.value()) {
    if (entry.second.size() != schema.size()) {
      return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument(
          "GroupByExecutor 输入行列数与 Schema 不符"));
    }
    const bool exists = std::any_of(
        groups.begin(), groups.end(), [&](const RowEntry &group) {
          return SameGroup(group.second, entry.second, indexes);
        });
    if (!exists) groups.push_back(std::move(entry));
  }
  std::unique_ptr<RowSource> source =
      std::make_unique<MaterializedSource>(std::move(groups));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<std::unique_ptr<RowSource>> OrderByExecutor::Execute(
    const OrderByPlan &plan, std::unique_ptr<RowSource> child,
    const Schema &schema) const {
  std::vector<std::pair<std::size_t, OrderDirection>> keys;
  keys.reserve(plan.keys.size());
  for (const auto &key : plan.keys) {
    auto index = schema.FindColumnIndex(key.column);
    if (!index.ok()) return Result<std::unique_ptr<RowSource>>(index.status());
    keys.emplace_back(index.value(), key.direction);
  }
  auto rows = Materialize(std::move(child), "OrderByExecutor");
  if (!rows.ok()) return Result<std::unique_ptr<RowSource>>(rows.status());
  for (const auto &entry : rows.value()) {
    if (entry.second.size() != schema.size()) {
      return Result<std::unique_ptr<RowSource>>(Status::InvalidArgument(
          "OrderByExecutor 输入行列数与 Schema 不符"));
    }
  }
  std::stable_sort(rows.value().begin(), rows.value().end(),
                   [&](const RowEntry &left, const RowEntry &right) {
                     for (const auto &[index, direction] : keys) {
                       const int comparison =
                           CompareValue(left.second[index], right.second[index]);
                       if (comparison == 0) continue;
                       return direction == OrderDirection::Asc
                                  ? comparison < 0
                                  : comparison > 0;
                     }
                     return false;
                   });
  std::unique_ptr<RowSource> source =
      std::make_unique<MaterializedSource>(std::move(rows.value()));
  return Result<std::unique_ptr<RowSource>>(std::move(source));
}

Result<ExecutionResult> DeleteExecutor::Execute(const DeletePlan &plan,
                                                Transaction *transaction) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("DeleteExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", metadata.status()));
  HeapTable table(buffer_pool_, *metadata.value());

  std::optional<std::size_t> predicate_column;
  std::optional<IndexMetadata> lookup_index;
  if (plan.where.has_value()) {
    auto index = metadata.value()->schema.FindColumnIndex(plan.where->column);
    if (!index.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", index.status()));
    predicate_column = index.value();
    if (metadata.value()->schema.At(index.value()).type != plan.where->value.type()) {
      return Result<ExecutionResult>(Status::TypeMismatch("Delete WHERE 列和值类型不匹配: " +
                                                           plan.where->column));
    }
    if (plan.where->kind == PredicateKind::Equal &&
        plan.where->value.IsInt()) {
      for (const auto &index_metadata :
           catalog_->ListTableIndexes(plan.table_name)) {
        if (index_metadata.column_name == plan.where->column) {
          lookup_index = index_metadata;
          break;
        }
      }
    }
  }

  ExecutionResult result;
  IndexManager index_manager(catalog_, buffer_pool_);
  const auto delete_entry = [&](const RID &rid, const Row &row) -> Status {
    const bool matches = !predicate_column.has_value() ||
                         row[predicate_column.value()] == plan.where->value;
    if (!matches) return Status::Ok();
    auto index_status =
        index_manager.OnDelete(plan.table_name, row, rid, transaction);
    if (!index_status.ok()) return Contextualize("DeleteExecutor", index_status);
    auto status = table.DeleteRow(rid, transaction);
    if (!status.ok()) {
      if (transaction != nullptr) {
        transaction->MarkFailed();
      } else {
        (void)index_manager.OnInsert(plan.table_name, row, rid);
      }
      return Contextualize("DeleteExecutor", status);
    }
    result.rids.push_back(rid);
    ++result.affected_rows;
    return Status::Ok();
  };

  if (lookup_index.has_value()) {
    // 索引先返回候选 RID；HeapTable::GetRow 复核真实行后再同步维护全部索引并
    // 删除记录。这样数据定位不再执行 SeqScan，同时仍能抵御陈旧索引项。
    auto rids = index_manager.Lookup(lookup_index->name,
                                     plan.where->value.AsInt(), transaction);
    if (!rids.ok()) {
      return Result<ExecutionResult>(
          Contextualize("DeleteExecutor", rids.status()));
    }
    for (const auto &rid : rids.value()) {
      auto row = table.GetRow(rid);
      if (!row.ok()) {
        return Result<ExecutionResult>(
            Contextualize("DeleteExecutor", row.status()));
      }
      auto status = delete_entry(rid, row.value());
      if (!status.ok()) return Result<ExecutionResult>(status);
    }
    return Result<ExecutionResult>(std::move(result));
  }

  auto cursor = table.BeginScan();
  if (!cursor.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", cursor.status()));
  // 没有可用等值 INT 索引时保留完整扫描路径。
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) return Result<ExecutionResult>(Contextualize("DeleteExecutor", entry.status()));
    if (!entry.value().has_value()) break;
    auto row_entry = std::move(entry.value().value());
    auto status = delete_entry(row_entry.first, row_entry.second);
    if (!status.ok()) return Result<ExecutionResult>(status);
  }
  return Result<ExecutionResult>(std::move(result));
}

Result<ExecutionResult> UpdateExecutor::Execute(const UpdatePlan &plan,
                                                Transaction *transaction) const {
  if (buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(
        Status::InvalidArgument("UpdateExecutor 缺少 BufferPoolManager"));
  }
  auto metadata = RequireMetadata(catalog_, plan.table_name);
  if (!metadata.ok()) {
    return Result<ExecutionResult>(
        Contextualize("UpdateExecutor", metadata.status()));
  }
  const Schema &schema = metadata.value()->schema;
  std::vector<std::size_t> assignment_indexes;
  assignment_indexes.reserve(plan.assignments.size());
  for (const auto &assignment : plan.assignments) {
    auto index = schema.FindColumnIndex(assignment.column);
    if (!index.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", index.status()));
    }
    assignment_indexes.push_back(index.value());
  }
  std::optional<std::size_t> predicate_index;
  if (plan.where.has_value()) {
    auto index = schema.FindColumnIndex(plan.where->column);
    if (!index.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", index.status()));
    }
    predicate_index = index.value();
  }

  HeapTable table(buffer_pool_, *metadata.value());
  auto cursor = table.BeginScan();
  if (!cursor.ok()) {
    return Result<ExecutionResult>(Contextualize("UpdateExecutor", cursor.status()));
  }
  struct Replacement {
    RID old_rid;
    Row old_row;
    Row new_row;
  };
  std::vector<Replacement> replacements;
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) {
      return Result<ExecutionResult>(Contextualize("UpdateExecutor", entry.status()));
    }
    if (!entry.value().has_value()) break;
    auto original = std::move(entry.value().value());
    if (predicate_index.has_value() &&
        original.second[*predicate_index] != plan.where->value) {
      continue;
    }
    Row updated = original.second;
    for (std::size_t i = 0; i < plan.assignments.size(); ++i) {
      auto value = EvaluateUpdateExpression(plan.assignments[i].expression,
                                            original.second, schema);
      if (!value.ok()) {
        return Result<ExecutionResult>(
            Contextualize("UpdateExecutor", value.status()));
      }
      const auto &column = schema.At(assignment_indexes[i]);
      if (value.value().type() != column.type) {
        return Result<ExecutionResult>(Status::TypeMismatch(
            "UpdateExecutor: SET 值类型不匹配: " + column.name));
      }
      if (column.type == DataType::Varchar && column.length.has_value() &&
          value.value().AsVarchar().size() > *column.length) {
        return Result<ExecutionResult>(Status::InvalidArgument(
            "UpdateExecutor: SET 字符串超过列长度: " + column.name));
      }
      updated[assignment_indexes[i]] = std::move(value.value());
    }
    replacements.push_back(
        Replacement{original.first, original.second, std::move(updated)});
  }

  ExecutionResult result;
  IndexManager index_manager(catalog_, buffer_pool_);
  for (const auto &replacement : replacements) {
    auto new_rid = table.InsertRow(replacement.new_row, transaction);
    if (!new_rid.ok()) {
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", new_rid.status()));
    }
    auto index_status =
        index_manager.OnUpdate(plan.table_name, replacement.old_row,
                               replacement.old_rid, replacement.new_row,
                               new_rid.value(), transaction);
    if (!index_status.ok()) {
      if (transaction != nullptr) {
        transaction->MarkFailed();
      } else {
        (void)table.DeleteRow(new_rid.value());
      }
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", index_status));
    }
    auto delete_status = table.DeleteRow(replacement.old_rid, transaction);
    if (!delete_status.ok()) {
      if (transaction != nullptr) {
        transaction->MarkFailed();
      } else {
        (void)index_manager.OnUpdate(plan.table_name, replacement.new_row,
                                     new_rid.value(), replacement.old_row,
                                     replacement.old_rid);
        (void)table.DeleteRow(new_rid.value());
      }
      return Result<ExecutionResult>(
          Contextualize("UpdateExecutor", delete_status));
    }
    result.rids.push_back(new_rid.value());
    ++result.affected_rows;
  }
  return Result<ExecutionResult>(std::move(result));
}

Result<ExecutionResult> AlterTableExecutor::Execute(
    const AlterTablePlan &plan, Transaction *transaction) const {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(
        Status::InvalidArgument("AlterTableExecutor 缺少存储依赖"));
  }
  auto old_metadata_result = catalog_->GetTableMetadataCopy(plan.table_name);
  if (!old_metadata_result.ok()) {
    return Result<ExecutionResult>(
        Contextualize("AlterTableExecutor", old_metadata_result.status()));
  }
  const TableMetadata old_metadata = old_metadata_result.value();
  HeapTable old_table(buffer_pool_, old_metadata);
  auto scanned = old_table.Scan();
  if (!scanned.ok()) {
    return Result<ExecutionResult>(
        Contextualize("AlterTableExecutor", scanned.status()));
  }
  std::vector<Row> old_rows;
  old_rows.reserve(scanned.value().size());
  for (auto &entry : scanned.value()) old_rows.push_back(std::move(entry.second));

  TableMetadata replacement = old_metadata;
  std::vector<Column> columns = old_metadata.schema.columns();
  std::vector<Row> replacement_rows = old_rows;
  if (plan.action == AlterTableAction::RenameTable) {
    replacement.name = plan.new_name;
  } else if (plan.action == AlterTableAction::RenameColumn) {
    auto index = old_metadata.schema.FindColumnIndex(plan.column_name);
    if (!index.ok()) return Result<ExecutionResult>(index.status());
    columns[index.value()].name = plan.new_name;
    replacement.schema = Schema(std::move(columns));
  } else {
    columns.push_back(plan.column);
    replacement.schema = Schema(std::move(columns));
    const Value fill = plan.default_value.has_value()
                           ? *plan.default_value
                           : plan.column.default_value.has_value()
                                 ? *plan.column.default_value
                                 : Value{};
    for (auto &row : replacement_rows) row.push_back(fill);
  }
  replacement.first_data_page_id = INVALID_PAGE_ID;

  // 先编码所有新行，避免销毁旧表后才发现行过大或类型错误。
  for (const auto &row : replacement_rows) {
    auto encoded = RowCodec::Encode(replacement.schema, row);
    if (!encoded.ok()) return Result<ExecutionResult>(encoded.status());
  }

  const auto old_indexes = catalog_->ListTableIndexes(plan.table_name);
  std::vector<IndexMetadata> replacement_indexes = old_indexes;
  for (auto &index : replacement_indexes) {
    index.table_name = replacement.name;
    if (plan.action == AlterTableAction::RenameColumn &&
        index.column_name == plan.column_name) {
      index.column_name = plan.new_name;
    }
    index.root_page_id = INVALID_PAGE_ID;
  }
  IndexManager index_manager(catalog_, buffer_pool_);

  const auto install = [&](const TableMetadata &metadata,
                           const std::vector<Row> &rows,
                           const std::vector<IndexMetadata> &indexes) -> Status {
    auto status = catalog_->CreateTable(metadata);
    if (!status.ok()) return status;
    auto installed = catalog_->GetTableMetadataCopy(metadata.name);
    if (!installed.ok()) return installed.status();
    HeapTable table(buffer_pool_, installed.value());
    for (const auto &row : rows) {
      auto rid = table.InsertRow(row);
      if (!rid.ok()) return rid.status();
    }
    for (const auto &index : indexes) {
      status = index_manager.CreateIndex(index.name, metadata.name,
                                         index.column_name, index.is_unique);
      if (!status.ok()) return status;
    }
    return Status::Ok();
  };

  const auto remove = [&](std::string_view table_name) -> Status {
    for (const auto &index : catalog_->ListTableIndexes(table_name)) {
      auto status = index_manager.DropIndex(index.name);
      if (!status.ok()) return status;
    }
    auto metadata = catalog_->GetTableMetadataCopy(table_name);
    if (!metadata.ok()) return metadata.status();
    HeapTable table(buffer_pool_, metadata.value());
    auto status = table.Destroy();
    if (!status.ok()) return status;
    return catalog_->DropTable(table_name);
  };

  auto removed = remove(plan.table_name);
  if (!removed.ok()) {
    if (transaction != nullptr) transaction->MarkFailed();
    return Result<ExecutionResult>(Contextualize("AlterTableExecutor", removed));
  }
  auto installed = install(replacement, replacement_rows, replacement_indexes);
  if (!installed.ok()) {
    if (catalog_->HasTable(replacement.name)) (void)remove(replacement.name);
    auto restored = install(old_metadata, old_rows, old_indexes);
    if (transaction != nullptr) transaction->MarkFailed();
    if (!restored.ok()) {
      return Result<ExecutionResult>(Status::IOError(
          "AlterTableExecutor: " + installed.message() +
          "; 恢复旧表也失败: " + restored.message()));
    }
    return Result<ExecutionResult>(Contextualize("AlterTableExecutor", installed));
  }

  ExecutionResult result;
  result.affected_rows = static_cast<std::uint64_t>(replacement_rows.size());
  return Result<ExecutionResult>(std::move(result));
}

Result<ExecutionEngine::SourcePlan> ExecutionEngine::BuildSource(
    const SelectPlan &plan, const ExecutionContext &context) const {
  if (plan.root == nullptr) {
    return Result<SourcePlan>(Status::InvalidArgument("ExecutionEngine SELECT 缺少根算子"));
  }

  SubqueryRunner subquery_runner =
      [this, &context](const SelectStatement &statement)
          -> Result<ExecutionResult> {
    auto compiled = Planner().Build(statement, *catalog_);
    if (!compiled.ok()) return Result<ExecutionResult>(compiled.status());
    return Execute(compiled.value(), context);
  };

  std::function<Result<SourcePlan>(const std::shared_ptr<const PlanNode> &)> build_node;
  build_node = [&](const std::shared_ptr<const PlanNode> &node) -> Result<SourcePlan> {
    if (node == nullptr) return Result<SourcePlan>(Status::InvalidArgument("执行计划包含空算子"));
    return std::visit(
        [&](const auto &operation) -> Result<SourcePlan> {
          using Type = std::decay_t<decltype(operation)>;
          if constexpr (std::is_same_v<Type, SeqScanPlan>) {
            // 叶子节点产生数据；Filter/Project 节点包装 child，最终组成拉取式流水线。
            SeqScanExecutor executor(catalog_, buffer_pool_);
            auto source = executor.Execute(operation);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            auto metadata = RequireMetadata(catalog_, operation.table_name);
            if (!metadata.ok()) return Result<SourcePlan>(metadata.status());
            std::vector<std::string> names;
            for (const auto &column : metadata.value()->schema.columns()) names.push_back(column.name);
            return Result<SourcePlan>(SourcePlan{std::move(source.value()), std::move(names),
                                                 metadata.value()->schema});
          } else if constexpr (std::is_same_v<Type, IndexScanPlan>) {
            if (!operation.key.IsInt()) {
              return Result<SourcePlan>(Status::TypeMismatch(
                  "IndexScan 只支持 INT 等值键"));
            }
            auto index_metadata = catalog_->FindIndex(operation.index_name);
            if (!index_metadata.ok()) {
              return Result<SourcePlan>(index_metadata.status());
            }
            if (index_metadata.value()->table_name != operation.table_name ||
                index_metadata.value()->column_name != operation.column_name) {
              return Result<SourcePlan>(Status::InvalidArgument(
                  "IndexScan 索引元数据与计划不匹配"));
            }
            IndexManager index_manager(catalog_, buffer_pool_);
            auto rids = index_manager.Lookup(operation.index_name,
                                             operation.key.AsInt());
            if (!rids.ok()) return Result<SourcePlan>(rids.status());
            auto metadata = RequireMetadata(catalog_, operation.table_name);
            if (!metadata.ok()) return Result<SourcePlan>(metadata.status());
            HeapTable table(buffer_pool_, *metadata.value());
            std::vector<RowEntry> rows;
            rows.reserve(rids.value().size());
            for (const auto &rid : rids.value()) {
              auto row = table.GetRow(rid);
              if (!row.ok()) return Result<SourcePlan>(row.status());
              rows.emplace_back(rid, std::move(row.value()));
            }
            std::vector<std::string> names;
            for (const auto &column : metadata.value()->schema.columns()) {
              names.push_back(column.name);
            }
            std::unique_ptr<RowSource> source =
                std::make_unique<MaterializedSource>(std::move(rows));
            return Result<SourcePlan>(
                SourcePlan{std::move(source), std::move(names), metadata.value()->schema});
          } else if constexpr (std::is_same_v<Type, FilterPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            FilterExecutor executor;
            auto source = executor.Execute(operation, std::move(child.value().source),
                                           child.value().schema,
                                           subquery_runner);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{std::move(source.value()),
                                                 std::move(child.value().column_names),
                                                 std::move(child.value().schema)});
          } else if constexpr (std::is_same_v<Type, GroupByPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            GroupByExecutor executor;
            auto source = executor.Execute(operation,
                                           std::move(child.value().source),
                                           child.value().schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{
                std::move(source.value()),
                std::move(child.value().column_names),
                std::move(child.value().schema)});
          } else if constexpr (std::is_same_v<Type, OrderByPlan>) {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            OrderByExecutor executor;
            auto source = executor.Execute(operation,
                                           std::move(child.value().source),
                                           child.value().schema);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            return Result<SourcePlan>(SourcePlan{
                std::move(source.value()),
                std::move(child.value().column_names),
                std::move(child.value().schema)});
          } else if constexpr (std::is_same_v<Type, JoinPlan>) {
            auto left = build_node(operation.left);
            if (!left.ok()) return Result<SourcePlan>(left.status());
            auto right = build_node(operation.right);
            if (!right.ok()) return Result<SourcePlan>(right.status());
            const std::size_t left_width = left.value().schema.size();
            const std::size_t right_width = right.value().schema.size();
            auto source = BuildJoinSource(std::move(left.value().source),
                                          std::move(right.value().source),
                                          operation, left_width, right_width);
            if (!source.ok()) return Result<SourcePlan>(source.status());
            std::vector<Column> columns = left.value().schema.columns();
            const auto &right_columns = right.value().schema.columns();
            columns.insert(columns.end(), right_columns.begin(), right_columns.end());
            auto names = std::move(left.value().column_names);
            names.insert(names.end(), right.value().column_names.begin(),
                         right.value().column_names.end());
            return Result<SourcePlan>(SourcePlan{std::move(source.value()),
                                                 std::move(names),
                                                 Schema(std::move(columns))});
          } else {
            auto child = build_node(operation.child);
            if (!child.ok()) return Result<SourcePlan>(child.status());
            Result<std::unique_ptr<RowSource>> source(
                Status::InternalError("Project 源未初始化"));
            const bool expression_projection =
                !operation.select_all && operation.input_indexes.empty() &&
                plan.projection_expressions.size() == operation.columns.size();
            if (expression_projection) {
              std::unique_ptr<RowSource> projected =
                  std::make_unique<ExpressionProjectSource>(
                      std::move(child.value().source),
                      plan.projection_expressions, child.value().schema,
                      subquery_runner);
              source = Result<std::unique_ptr<RowSource>>(std::move(projected));
            } else {
              ProjectExecutor executor;
              source = executor.Execute(operation,
                                        std::move(child.value().source),
                                        child.value().schema);
            }
            if (!source.ok()) return Result<SourcePlan>(source.status());
            std::vector<std::string> names;
            if (operation.select_all) {
              names = child.value().column_names;
            } else {
              names = operation.columns;
            }
            std::vector<Column> columns;
            const auto &indexes = operation.input_indexes;
            if (operation.select_all) {
              columns = child.value().schema.columns();
            } else for (std::size_t i = 0; i < names.size(); ++i) {
              Column column;
              if (expression_projection) {
                const DataType type = i < plan.output_types.size() &&
                                              plan.output_types[i] == PlanValueType::Varchar
                                          ? DataType::Varchar : DataType::Int;
                column = Column(names[i], type);
              } else {
                std::size_t index = i < indexes.size() ? indexes[i]
                    : child.value().schema.FindColumnIndex(operation.columns[i]).value();
                column = child.value().schema.At(index);
              }
              column.name = names[i];
              columns.push_back(std::move(column));
            }
            return Result<SourcePlan>(SourcePlan{std::move(source.value()), std::move(names),
                                                 Schema(std::move(columns))});
          }
        },
        node->operation);
  };

  const auto *project = std::get_if<ProjectPlan>(&plan.root->operation);
  if (project == nullptr) {
    return Result<SourcePlan>(Status::InvalidArgument("SELECT 根算子必须是 Project"));
  }

  SourcePlan result;
  if (plan.has_aggregates || plan.having != nullptr) {
    const PlanNode *cursor = project->child.get();
    const OrderByPlan *order = nullptr;
    const GroupByPlan *group_by = nullptr;
    if (cursor != nullptr) {
      order = std::get_if<OrderByPlan>(&cursor->operation);
      if (order != nullptr) cursor = order->child.get();
    }
    if (cursor != nullptr) {
      group_by = std::get_if<GroupByPlan>(&cursor->operation);
      if (group_by != nullptr) cursor = group_by->child.get();
    }
    if (cursor == nullptr) {
      return Result<SourcePlan>(Status::InvalidArgument("聚合计划缺少数据源"));
    }
    auto input = build_node(std::make_shared<PlanNode>(cursor->operation));
    if (!input.ok()) return input;
    auto rows = Materialize(std::move(input.value().source), "AggregateExecutor");
    if (!rows.ok()) return Result<SourcePlan>(rows.status());
    std::vector<std::size_t> group_indexes;
    if (group_by != nullptr) {
      for (const auto &name : group_by->columns) {
        auto index = input.value().schema.FindColumnIndex(name);
        if (!index.ok()) return Result<SourcePlan>(index.status());
        group_indexes.push_back(index.value());
      }
    }
    std::vector<std::vector<RowEntry>> groups;
    for (auto &entry : rows.value()) {
      auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &group) {
        return !group.empty() && SameGroup(group.front().second, entry.second, group_indexes);
      });
      if (found == groups.end()) groups.push_back({std::move(entry)});
      else found->push_back(std::move(entry));
    }
    if (groups.empty() && group_indexes.empty()) groups.emplace_back();
    if (order != nullptr) {
      std::vector<std::pair<std::size_t, OrderDirection>> keys;
      for (const auto &key : order->keys) {
        auto index = input.value().schema.FindColumnIndex(key.column);
        if (!index.ok()) return Result<SourcePlan>(index.status());
        keys.emplace_back(index.value(), key.direction);
      }
      std::stable_sort(groups.begin(), groups.end(), [&](const auto &left, const auto &right) {
        if (left.empty() || right.empty()) return false;
        for (const auto &[index, direction] : keys) {
          const int compared = CompareValue(left.front().second[index], right.front().second[index]);
          if (compared != 0) return direction == OrderDirection::Asc ? compared < 0 : compared > 0;
        }
        return false;
      });
    }
    std::vector<RowEntry> output;
    for (auto &group : groups) {
      Row empty(input.value().schema.size(), Value{});
      const Row &representative = group.empty() ? empty : group.front().second;
      std::vector<const Row *> members;
      for (const auto &entry : group) members.push_back(&entry.second);
      if (plan.having != nullptr) {
        auto condition = EvaluateExpression(
            plan.having, representative, input.value().schema, &members,
            &subquery_runner);
        if (!condition.ok()) return Result<SourcePlan>(condition.status());
        if (condition.value().kind != EvalValue::Kind::Bool ||
            !condition.value().boolean) continue;
      }
      Row projected;
      for (const auto &expression : plan.projection_expressions) {
        auto value = EvaluateExpression(
            expression, representative, input.value().schema, &members,
            &subquery_runner);
        if (!value.ok()) return Result<SourcePlan>(value.status());
        auto stored = ToStorageValue(value.value());
        if (!stored.ok()) return Result<SourcePlan>(stored.status());
        projected.push_back(std::move(stored.value()));
      }
      output.emplace_back(group.empty() ? RID{} : group.front().first,
                          std::move(projected));
    }
    std::vector<std::string> names = project->columns;
    std::vector<Column> columns;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i < plan.projection_aliases.size() && !plan.projection_aliases[i].empty()) {
        names[i] = plan.projection_aliases[i];
      }
      const DataType type = i < plan.output_types.size() &&
                                    plan.output_types[i] == PlanValueType::Varchar
                                ? DataType::Varchar : DataType::Int;
      columns.emplace_back(names[i], type);
    }
    result = SourcePlan{std::make_unique<MaterializedSource>(std::move(output)),
                        std::move(names), Schema(std::move(columns))};
  } else {
    auto built = build_node(plan.root);
    if (!built.ok()) return built;
    result = std::move(built.value());
    for (std::size_t i = 0; i < result.column_names.size() &&
                            i < plan.projection_aliases.size(); ++i) {
      if (!plan.projection_aliases[i].empty()) result.column_names[i] = plan.projection_aliases[i];
    }
  }
  if (plan.distinct) result.source = std::make_unique<DistinctSource>(std::move(result.source));
  if (plan.limit.has_value()) result.source = std::make_unique<LimitSource>(std::move(result.source), *plan.limit);
  return Result<SourcePlan>(std::move(result));
}

Result<ExecutionResult> ExecutionEngine::Execute(const Plan &plan) const {
  return Execute(plan, ExecutionContext{});
}

Result<ExecutionResult> ExecutionEngine::Execute(const Plan &plan,
                                                 const ExecutionContext &context) const {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Result<ExecutionResult>(Status::InvalidArgument("ExecutionEngine 缺少存储依赖"));
  }
  // DDL/DML 计划的总分派点。新增 UpdatePlan、用户管理 Plan 等都在此接入 Executor。
  return std::visit(
      [&](const auto &operation) -> Result<ExecutionResult> {
        using Type = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Type, CreateTablePlan>) {
          return CreateTableExecutor(catalog_).Execute(operation);
        } else if constexpr (std::is_same_v<Type, InsertPlan>) {
          return InsertExecutor(catalog_, buffer_pool_).Execute(operation,
                                                                  context.transaction);
        } else if constexpr (std::is_same_v<Type, SelectPlan>) {
          auto source = BuildSource(operation, context);
          if (!source.ok()) return Result<ExecutionResult>(source.status());
          ExecutionResult result;
          result.column_names = std::move(source.value().column_names);
          // 执行引擎作为管道消费者，持续 Next()，直到 optional 为空表示 EOF。
          while (true) {
            auto entry = source.value().source->Next();
            if (!entry.ok()) return Result<ExecutionResult>(Contextualize("ExecutionEngine", entry.status()));
            if (!entry.value().has_value()) break;
            auto row_entry = std::move(entry.value().value());
            result.rids.push_back(row_entry.first);
            result.rows.push_back(std::move(row_entry.second));
          }
          return Result<ExecutionResult>(std::move(result));
        } else if constexpr (std::is_same_v<Type, DropTablePlan>) {
          auto metadata = catalog_->GetTableMetadata(operation.table_name);
          if (!metadata.ok()) return Result<ExecutionResult>(metadata.status());
          const auto table_metadata = *metadata.value();
          const auto indexes = catalog_->ListTableIndexes(operation.table_name);
          IndexManager index_manager(catalog_, buffer_pool_);
          for (const auto &index : indexes) {
            auto status = index_manager.DropIndex(index.name);
            if (!status.ok()) {
              return Result<ExecutionResult>(Contextualize("DropTableExecutor", status));
            }
          }
          HeapTable table(buffer_pool_, table_metadata);
          auto status = table.Destroy();
          if (!status.ok()) {
            return Result<ExecutionResult>(Contextualize("DropTableExecutor", status));
          }
          status = catalog_->DropTable(operation.table_name);
          if (!status.ok()) {
            return Result<ExecutionResult>(Contextualize("DropTableExecutor", status));
          }
          return Result<ExecutionResult>(ExecutionResult{});
        } else if constexpr (std::is_same_v<Type, CreateIndexPlan>) {
          auto status = IndexManager(catalog_, buffer_pool_)
                            .CreateIndex(operation.index_name,
                                         operation.table_name,
                                         operation.column_name,
                                         operation.is_unique);
          if (!status.ok()) {
            return Result<ExecutionResult>(
                Contextualize("CreateIndexExecutor", status));
          }
          return Result<ExecutionResult>(ExecutionResult{});
        } else if constexpr (std::is_same_v<Type, DropIndexPlan>) {
          auto status =
              IndexManager(catalog_, buffer_pool_).DropIndex(operation.index_name);
          if (!status.ok()) {
            return Result<ExecutionResult>(
                Contextualize("DropIndexExecutor", status));
          }
          return Result<ExecutionResult>(ExecutionResult{});
        } else if constexpr (std::is_same_v<Type, ExplainPlan>) {
          ExecutionResult result;
          result.column_names = {"plan"};
          result.rows.push_back({Value(ToString(operation.select))});
          return Result<ExecutionResult>(std::move(result));
        } else if constexpr (std::is_same_v<Type, UpdatePlan>) {
          return UpdateExecutor(catalog_, buffer_pool_).Execute(operation,
                                                                  context.transaction);
        } else if constexpr (std::is_same_v<Type, AlterTablePlan>) {
          return AlterTableExecutor(catalog_, buffer_pool_)
              .Execute(operation, context.transaction);
        } else if constexpr (std::is_same_v<Type, BeginPlan>) {
          return Result<ExecutionResult>(Status::NotImplemented(
              "BEGIN 必须由 DatabaseEngine 事务协调器处理"));
        } else if constexpr (std::is_same_v<Type, CommitPlan>) {
          return Result<ExecutionResult>(Status::NotImplemented(
              "COMMIT 必须由 DatabaseEngine 事务协调器处理"));
        } else if constexpr (std::is_same_v<Type, RollbackPlan>) {
          return Result<ExecutionResult>(Status::NotImplemented(
              "ROLLBACK 必须由 DatabaseEngine 事务协调器处理"));
        } else {
          return DeleteExecutor(catalog_, buffer_pool_).Execute(operation,
                                                                  context.transaction);
        }
      },
      plan);
}

}  // namespace oursql
