#include "oursql/optimizer/optimizer.h"

#include <utility>
#include <variant>

namespace oursql {

namespace {

// 冻结计划形态：SELECT 的根算子必须是 ProjectPlan；其下只允许出现
// OrderBy?、GroupBy?、Filter? 和 SeqScanPlan。GroupBy/OrderBy 是编译层
// 扩展算子，Filter 只允许直接包住 SeqScan。执行器依赖根 Project 取得投影
// 列，因此所有规则都必须保留这一外层合同。
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
    if (const auto *outer =
            std::get_if<ProjectPlan>(&select->root->operation)) {
      if (outer->child != nullptr) {
        if (const auto *inner =
                std::get_if<ProjectPlan>(&outer->child->operation)) {
          if (ProjectionCovers(*inner, *outer)) {
            select->root = std::make_shared<PlanNode>(
                ProjectPlan{inner->child, outer->columns,
                            outer->select_all});
            ++stats.rule_hits["R1:冗余内层Project裁剪"];
            changed = true;
          }
        }
      }
    }

    // R2：等值 INT 谓词存在匹配索引时，把 SeqScan 替换为 IndexScan。
    // Filter 节点仍保留，执行时会对索引返回的行再次求值，保证正确性。
    if (const auto *project =
            std::get_if<ProjectPlan>(&select->root->operation)) {
      if (project->child != nullptr) {
        if (const auto *filter =
                std::get_if<FilterPlan>(&project->child->operation)) {
          if (filter->child != nullptr) {
            if (const auto *scan =
                    std::get_if<SeqScanPlan>(&filter->child->operation)) {
              if (filter->predicate.value.IsInt()) {
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
                      FilterPlan{index_scan, filter->predicate});
                  select->root = std::make_shared<PlanNode>(
                      ProjectPlan{new_filter, project->columns,
                                  project->select_all});
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
        "Project(OrderBy?(GroupBy?(Filter?(SeqScan|IndexScan))))"));
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
