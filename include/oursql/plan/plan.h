#pragma once

#include "oursql/ast/statement.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace oursql {

struct CreateTablePlan {
  std::string table_name;
  Schema schema;
};

struct InsertPlan {
  std::string table_name;
  std::vector<Value> values;
};

struct SeqScanPlan {
  std::string table_name;
};

struct PlanNode;

struct FilterPlan {
  std::shared_ptr<const PlanNode> child;
  Predicate predicate;
};

struct ProjectPlan {
  std::shared_ptr<const PlanNode> child;
  std::vector<std::string> columns;
  bool select_all{false};
};

struct PlanNode {
  using Operation = std::variant<SeqScanPlan, FilterPlan, ProjectPlan>;

  explicit PlanNode(Operation operation_value) : operation(std::move(operation_value)) {}
  Operation operation;
};

struct SelectPlan {
  std::string table_name;
  std::shared_ptr<const PlanNode> root;
};

struct DeletePlan {
  std::string table_name;
  std::optional<Predicate> where;
};

using Plan = std::variant<CreateTablePlan, InsertPlan, SelectPlan, DeletePlan>;

// 调用者：调试器或测试；作用：以稳定格式打印逻辑计划；返回：计划文本，不参与执行。
[[nodiscard]] std::string ToString(const Plan &plan);

}  // namespace oursql
