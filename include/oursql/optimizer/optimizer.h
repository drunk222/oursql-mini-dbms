#pragma once

#include "oursql/common/status.h"
#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace oursql {

struct OptimizationStats {
  // 调度器执行的总轮数。最后一轮通常是没有规则命中的稳定检查轮。
  std::size_t passes{0};
  // 规则名称 -> 累计命中次数，便于测试和答辩展示每条规则是否真正生效。
  std::map<std::string, std::size_t> rule_hits;
};

struct OptimizationResult {
  // 优化后的新 Plan；原 Plan 按值传入，调用者仍保留自己的副本。
  Plan plan;
  OptimizationStats stats;
};

// 调用者：Planner::Build（编译层计划后处理）与测试；作用：在冻结合同允许的
// 范围内改写逻辑计划，并校验 SELECT 的根节点是 Project，其下只出现允许的
// OrderBy/GroupBy/Filter/{SeqScan,IndexScan} 或合法 Join 子树；返回：计划
// 或错误状态。
//
// 当前规则集：
// - R1：裁剪被外层完全覆盖的冗余内层 Project；
// - R2：等值 INT 谓词存在匹配索引时生成 IndexScan，并保留 Filter 复核。
//
// 只允许不改变最终结果列、行来源和执行语义的重写。当前决策仍是规则式，
// 不基于表行数、页数、distinct count 或值频率做代价比较。本类属于编译层，
// storage/catalog/execution 不反向依赖它。
class Optimizer {
 public:
  // 兼容旧调用方，只返回优化后的计划。
  [[nodiscard]] Result<Plan> Optimize(Plan plan) const;
  // 调用者：Planner；作用：使用目标表索引定义尝试生成 IndexScan；返回：
  // 重写后的计划。索引列表为空时只运行不依赖索引的规则。
  [[nodiscard]] Result<Plan> Optimize(
      Plan plan, const std::vector<IndexMetadata> &indexes) const;
  // 调用者：需要观察优化过程的测试或展示代码；返回计划及规则调度统计。
  // rule_hits 会分别记录 R1、R2 的实际命中次数。
  [[nodiscard]] Result<OptimizationResult> OptimizeWithStats(Plan plan) const;
  [[nodiscard]] Result<OptimizationResult> OptimizeWithStats(
      Plan plan, const std::vector<IndexMetadata> &indexes) const;
};

}  // namespace oursql
