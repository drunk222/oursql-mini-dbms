#pragma once

#include "oursql/common/status.h"

namespace oursql {

class DiskManager;
class LogManager;

class RecoveryManager {
 public:
  RecoveryManager(LogManager *log_manager, DiskManager *disk_manager) noexcept
      : log_manager_(log_manager), disk_manager_(disk_manager) {}

  // Replays committed records and undoes the one incomplete transaction.
  [[nodiscard]] Status Recover();

 private:
  LogManager *log_manager_{nullptr};
  DiskManager *disk_manager_{nullptr};
};

}  // namespace oursql
