#include "oursql/execution/executor.h"

namespace oursql {

Status Executor::Execute(const Plan &, Catalog &) const {
  return Status::NotImplemented("执行器尚未实现");
}

}  // namespace oursql

