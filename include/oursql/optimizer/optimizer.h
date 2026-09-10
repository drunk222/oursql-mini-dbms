#pragma once

#include "oursql/common/status.h"
#include "oursql/plan/plan.h"

#include <string>

namespace oursql {

// 调用者：Planner::Build（编译层计划后处理）与测试；作用：在冻结合同允许的
// 范围内改写逻辑计划，并校验「SELECT 的计划树固定为 Project(Filter(SeqScan))」；
// 返回：计划或错误状态。
// 约束：只允许不改变冻结计划形态、也不改变执行结果的重写。当前规则集
// 至少包含「冗余内层 Project 裁剪」。本类属于编译层，数据库层
// （storage/catalog/execution）不依赖它。
class Optimizer {
 public:
  [[nodiscard]] Result<Plan> Optimize(Plan plan) const;
};

}  // namespace oursql
