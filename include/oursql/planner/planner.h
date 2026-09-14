#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"

// AST 到逻辑 Plan 的语义分析与计划生成入口。
//
// Planner 只通过 CatalogReader 读取表、列和索引定义，不登记表、不写数据、
// 不访问 BufferPool。Build() 完成语义检查后构造 Plan，并在 SELECT 路径上
// 调用 Optimizer 执行规则式 R1/R2 重写。
namespace oursql {

class Planner {
 public:
  // 调用者：SQL 编译管线。
  // 作用：把单条 AST 变成结构化 Plan，并完成表/列/别名/类型/约束等语义检查。
  // 处理顺序：先检查 AST 字段，再通过 CatalogReader 查表与列，构造 Plan；
  // 最后把 SELECT 计划交给 Optimizer 做 R1/R2 规则重写。
  // 返回：可执行或可诊断的逻辑计划，或带语句位置的语义错误。
  //
  // catalog 只提供只读查询。Planner 不登记表、不写数据、不访问 BufferPool；
  // 数据库副作用由 ExecutionEngine/Catalog/HeapTable 在计划执行阶段完成。
  [[nodiscard]] Result<Plan> Build(const Statement &statement, const CatalogView &catalog) const;
};

}  // namespace oursql
