#include "oursql/optimizer/optimizer.h"

#include <utility>
#include <variant>

// 计划优化器实现说明：
//
// 每轮按固定顺序尝试 R1、R2；任一规则改写计划就继续下一轮，直到整轮稳定。
// 规则必须保留 SELECT 根 Project 和执行器可消费的算子形态。如果输入非法或
// 规则无法修复，返回 InternalError，而不是把未知计划交给 Executor。
namespace oursql {

namespace {

// 判断 JOIN 子树是否为执行器认识的二元连接树。叶子只能是 SeqScan；
// 多表 JOIN 以左深 JoinPlan 表示，右侧每个节点对应一个显式 JOIN 子句。
bool IsJoinSource(const PlanNode &node) {
  if (std::holds_alternative<SeqScanPlan>(node.operation)) return true;
  const auto *join = std::get_if<JoinPlan>(&node.operation);
  return join != nullptr && join->left != nullptr && join->right != nullptr &&
         IsJoinSource(*join->left) && IsJoinSource(*join->right);
}

// 冻结计划形态：SELECT 的根算子必须是 ProjectPlan；其下只允许出现
// OrderBy?、GroupBy?、Filter?、SeqScan/IndexScan 或合法 Join 子树。
// GroupBy/OrderBy 负责数据组织，Filter 直接包住扫描叶子；Join 叶子必须是
// 可由执行器构造的二元连接树。执行器依赖根 Project 取得投影列，因此所有
// 规则都必须保留这一外层合同。
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
  if (const auto *join = std::get_if<JoinPlan>(&current->operation)) {
    return IsJoinSource(*current);
  }
  if (const auto *filter = std::get_if<FilterPlan>(&current->operation)) {
    return filter->child != nullptr &&
           (std::holds_alternative<SeqScanPlan>(
                filter->child->operation) ||
            std::holds_alternative<IndexScanPlan>(
                filter->child->operation));
  }
  return std::holds_alternative<SeqScanPlan>(current->operation) ||
         std::holds_alternative<IndexScanPlan>(current->operation);
}

// 判断内层 Project 是否覆盖外层 Project 所需的全部列。
// 该函数不访问 Catalog，因此只能依据 Plan 中已经保存的列集合做保守判断。
bool ProjectionCovers(const ProjectPlan &inner, const ProjectPlan &outer) {
  // 外层需要整行时，只有内层也保留整行才能证明列集合不变。当前函数拿不到
  // Catalog，无法在内层是窄投影时补齐未知列，因此不能安全裁剪。
  if (outer.select_all) return inner.select_all;
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

Result<OptimizationResult> Optimizer::OptimizeWithStats(
    Plan plan, const std::vector<IndexMetadata> &indexes) const {
  // 非 SELECT 计划当前没有适用规则，但仍返回统一的 stats，使调用方无需区分
  // 计划类型即可记录“执行了一次空优化”。
  OptimizationStats stats;
  auto *select = std::get_if<SelectPlan>(&plan);
  if (select == nullptr) {
    stats.passes = 1;
    return Result<OptimizationResult>(
        OptimizationResult{std::move(plan), std::move(stats)});
  }
  if (select->root == nullptr) {
    return Result<OptimizationResult>(
        Status::InvalidArgument("Optimizer: SELECT 计划缺少根算子"));
  }

  // 固定规则集 + 显式 changed 标志的不动点调度。
  // 每轮按固定顺序尝试规则；只要任一规则改写了计划，就继续下一轮。
  // 这样连续出现的冗余 Project 可以在后续轮次继续被处理。
  for (;;) {
    ++stats.passes;
    bool changed = false;

    // R1：连续 Project 的内层投影若覆盖外层所需列，可去掉内层 Project。
    // 规则只删除中间投影，不删除根 Project；外层 SELECT * 时只有内层也
    // 保留整行，才能证明删除后不会丢失输出列。
    if (const auto *outer =
            std::get_if<ProjectPlan>(&select->root->operation)) {
      if (outer->child != nullptr) {
        if (const auto *inner =
                std::get_if<ProjectPlan>(&outer->child->operation)) {
          if (ProjectionCovers(*inner, *outer)) {
            select->root = std::make_shared<PlanNode>(
                ProjectPlan{inner->child, outer->columns,
                            outer->select_all, outer->input_indexes});
            ++stats.rule_hits["R1:冗余内层Project裁剪"];
            changed = true;
          }
        }
      }
    }

    // R2：等值 INT 谓词存在匹配索引时，把 SeqScan 替换为 IndexScan。
    // 触发前提包括：
    // 1. 根仍是 Project，且其下是 Filter；
    // 2. Filter 的 Predicate 是“列 = INT 常量”；
    // 3. 索引属于同一张表且索引列与谓词列一致。
    // Filter 节点仍保留，执行时会复核索引返回的候选行，避免把索引本身
    // 当成最终语义来源；当前规则不做选择率估算，也不比较 SeqScan 成本。
    if (const auto *project =
            std::get_if<ProjectPlan>(&select->root->operation)) {
      if (project->child != nullptr) {
        if (const auto *filter =
                std::get_if<FilterPlan>(&project->child->operation)) {
          if (filter->child != nullptr) {
            if (const auto *scan =
                    std::get_if<SeqScanPlan>(&filter->child->operation)) {
              if (filter->predicate.kind == PredicateKind::Equal &&
                  filter->predicate.value.IsInt()) {
                for (const auto &index : indexes) {
                  if (index.table_name != scan->table_name ||
                      index.column_name != filter->predicate.column) {
                    continue;
                  }
                  auto index_scan = std::make_shared<PlanNode>(
                      IndexScanPlan{scan->table_name, index.name,
                                    index.column_name,
                                    filter->predicate.value});
                  auto new_filter = std::make_shared<PlanNode>(
                      FilterPlan{index_scan, filter->predicate,
                                 filter->expression});
                  select->root = std::make_shared<PlanNode>(
                      ProjectPlan{new_filter, project->columns,
                                  project->select_all,
                                  project->input_indexes});
                  ++stats.rule_hits["R2:等值谓词索引扫描"];
                  changed = true;
                  break;
                }
              }
            }
          }
        }
      }
    }

    // 整轮没有命中说明计划已稳定。passes 会包含这次稳定检查轮。
    if (!changed) break;
  }

  // 规则只能生成执行器允许的计划。若输入非法且规则无法修复，返回错误而不是
  // 把一个可能被 Executor 误解的 Plan 继续向下传递。
  if (!IsFrozenSelectShape(*select->root)) {
    return Result<OptimizationResult>(Status::InternalError(
        "Optimizer: SELECT 的计划树必须固定为 "
        "Project(OrderBy?(GroupBy?(Filter?(SeqScan|IndexScan)))|"
        "Join(SeqScan,SeqScan))"));
  }
  return Result<OptimizationResult>(
      OptimizationResult{std::move(plan), std::move(stats)});
}

Result<Plan> Optimizer::Optimize(Plan plan) const {
  // 兼容性入口：普通编译管线只关心 Plan，展示和测试可使用带统计信息的方法。
  auto result = OptimizeWithStats(std::move(plan), {});
  if (!result.ok()) return Result<Plan>(result.status());
  return Result<Plan>(std::move(result.value().plan));
}

Result<OptimizationResult> Optimizer::OptimizeWithStats(Plan plan) const {
  return OptimizeWithStats(std::move(plan), {});
}

Result<Plan> Optimizer::Optimize(
    Plan plan, const std::vector<IndexMetadata> &indexes) const {
  auto result = OptimizeWithStats(std::move(plan), indexes);
  if (!result.ok()) return Result<Plan>(result.status());
  return Result<Plan>(std::move(result.value().plan));
}

}  // namespace oursql
