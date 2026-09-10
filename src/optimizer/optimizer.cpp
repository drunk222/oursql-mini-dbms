#include "oursql/optimizer/optimizer.h"

#include <utility>
#include <variant>

namespace oursql {

namespace {

// 冻结计划形态：SELECT 的根算子必须是 ProjectPlan；其子节点只允许是
// FilterPlan（没有 WHERE 时省略）或 SeqScanPlan；FilterPlan 的子节点必须是
// SeqScanPlan。执行器依赖该形态取得投影列，因此任何改变形态的重写都不允许。
bool IsFrozenSelectShape(const PlanNode &node) {
  const auto *project = std::get_if<ProjectPlan>(&node.operation);
  if (project == nullptr || project->child == nullptr) return false;
  const PlanNode *current = project->child.get();
  // GROUP BY / ORDER BY 是编译层扩展算子，允许出现在 Project 与数据源之间。
  while (current != nullptr) {
    if (const auto *order = std::get_if<OrderByPlan>(&current->operation)) {
      current = order->child.get();
      continue;
    }
    if (const auto *group = std::get_if<GroupByPlan>(&current->operation)) {
      current = group->child.get();
      continue;
    }
    break;
  }
  if (current == nullptr) return false;
  if (const auto *filter = std::get_if<FilterPlan>(&current->operation)) {
    return filter->child != nullptr &&
           std::holds_alternative<SeqScanPlan>(filter->child->operation);
  }
  return std::holds_alternative<SeqScanPlan>(current->operation);
}

bool ProjectionCovers(const ProjectPlan &inner, const ProjectPlan &outer) {
  if (inner.select_all) return true;
  for (const auto &outer_column : outer.columns) {
    bool found = false;
    for (const auto &inner_column : inner.columns) {
      if (outer_column == inner_column) {
        found = true;
        break;
      }
    }
    if (!found) return false;
  }
  return true;
}

}  // namespace

Result<Plan> Optimizer::Optimize(Plan plan) const {
  auto *select = std::get_if<SelectPlan>(&plan);
  if (select == nullptr) return Result<Plan>(std::move(plan));
  if (select->root == nullptr) {
    return Result<Plan>(
        Status::InvalidArgument("Optimizer: SELECT 计划缺少根算子"));
  }
  // 规则：连续 Project 的内层投影若覆盖外层所需列，可去掉内层 Project。
  // 该规则不改变 Project(Filter(SeqScan)) 冻结形态，也不改变最终列集。
  if (const auto *outer =
          std::get_if<ProjectPlan>(&select->root->operation)) {
    if (outer->child != nullptr) {
      if (const auto *inner =
              std::get_if<ProjectPlan>(&outer->child->operation)) {
        if (ProjectionCovers(*inner, *outer)) {
          select->root = std::make_shared<PlanNode>(
              ProjectPlan{inner->child, outer->columns, outer->select_all});
        }
      }
    }
  }
  if (!IsFrozenSelectShape(*select->root)) {
    return Result<Plan>(Status::InternalError(
        "Optimizer: SELECT 的计划树必须固定为 Project(Filter(SeqScan))"));
  }
  // 当前可安全执行的规则是「冗余内层 Project 裁剪」；根 Project 永不删除，
  // 因此冻结形态和 Executor 的投影列来源保持不变。
  return Result<Plan>(std::move(plan));
}

}  // namespace oursql
