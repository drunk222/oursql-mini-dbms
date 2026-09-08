#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/plan/plan.h"
#include "oursql/storage/storage.h"

namespace oursql {

class Executor {
 public:
  // 调用者：命令行入口或事务控制器；作用：执行逻辑计划；返回：当前阶段未实现时给出明确错误。
  [[nodiscard]] Status Execute(const Plan &plan, Catalog &catalog) const;
};

}  // namespace oursql

