#pragma once

#include "oursql/common/status.h"
#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// 计划级规则优化器。
//
// Optimizer 只接收结构化 Plan，通过固定点调度执行 R1 内层 Project 裁剪和
// R2 等值 INT 谓词索引选择。它不解析 SQL、不访问 Catalog 页面，也不执行
// 数据读写；最终必须保留 Executor 可以消费的计划形态。
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

// 数据库编排层根据真实索引内容生成的点查成本估算。Optimizer 只负责比较
// 两条访问路径并改写 Plan，不直接读取 HeapTable 或 B+ 树。
struct IndexScanCostEstimate {
  std::string index_name;
  std::int64_t key{0};
  std::uint64_t total_entries{0};
  std::uint64_t matching_entries{0};
  double seq_scan_cost{0.0};
  double index_scan_cost{0.0};
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
  // 调用者：DatabaseEngine；作用：用真实选择率在已生成的 IndexScan 与
  // SeqScan 之间做成本选择。小表继续保留原索引规则，避免统计噪声主导计划。
  [[nodiscard]] Result<OptimizationResult> OptimizeWithCost(
      Plan plan, const IndexScanCostEstimate &estimate) const;
};

}  // namespace oursql
