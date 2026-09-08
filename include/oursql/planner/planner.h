#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"

namespace oursql {

class Planner {
 public:
  // 调用者：SQL 编译管线；作用：语义检查并把 AST 变成逻辑计划；返回：逻辑计划或语义错误。
  [[nodiscard]] Result<Plan> Build(const Statement &statement, const CatalogView &catalog) const;
};

}  // namespace oursql
