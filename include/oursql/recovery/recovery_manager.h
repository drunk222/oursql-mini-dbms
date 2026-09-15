#pragma once

#include "oursql/common/status.h"

namespace oursql {

class DiskManager;
class LogManager;

class RecoveryManager {
 public:
  // 调用者：DatabaseEngine；作用：保存 WAL 和主数据库文件依赖；返回：恢复协调器对象。
  RecoveryManager(LogManager *log_manager, DiskManager *disk_manager) noexcept
      : log_manager_(log_manager), disk_manager_(disk_manager) {}

  // 调用者：数据库启动流程；作用：扫描 WAL，Redo 已提交修改并 Undo 未完成事务；返回：恢复状态。
  [[nodiscard]] Status Recover();

 private:
  LogManager *log_manager_{nullptr};
  DiskManager *disk_manager_{nullptr};
};

}  // namespace oursql
