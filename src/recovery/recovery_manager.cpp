#include "oursql/recovery/recovery_manager.h"

#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace oursql {

namespace {

Status WriteImage(DiskManager *disk_manager, page_id_t page_id,
                  const std::vector<std::byte> &image) {
  if (disk_manager == nullptr || image.size() != Page::kSize) {
    return Status::InvalidArgument("recovery requires one full page image");
  }
  Page page;
  std::copy(image.begin(), image.end(), page.Data());
  return disk_manager->WritePage(page_id, page);
}

Status DeallocateIdempotent(DiskManager *disk_manager, page_id_t page_id) {
  auto status = disk_manager->DeallocatePage(page_id);
  return status.code() == ErrorCode::AlreadyExists ? Status::Ok() : status;
}

}  // namespace

Status RecoveryManager::Recover() {
  if (log_manager_ == nullptr || disk_manager_ == nullptr) {
    return Status::InvalidArgument("RecoveryManager requires LogManager and DiskManager");
  }
  auto scan = log_manager_->Scan();
  if (!scan.ok()) return Status::IOError("WAL recovery scan failed: " + scan.status().message());

  std::unordered_set<txn_id_t> committed;
  std::unordered_set<txn_id_t> terminal;
  std::unordered_map<txn_id_t, std::vector<const LogRecord *>> by_transaction;
  for (const auto &record : scan.value()) {
    if (record.txn_id != kInvalidTxnId) by_transaction[record.txn_id].push_back(&record);
    if (record.type == LogRecordType::Commit) {
      committed.insert(record.txn_id);
      terminal.insert(record.txn_id);
    } else if (record.type == LogRecordType::Abort) {
      terminal.insert(record.txn_id);
    }
  }

  // Redo committed page images in log order.
  for (const auto &record : scan.value()) {
    if (committed.find(record.txn_id) == committed.end()) continue;
    if (record.type == LogRecordType::PageUpdate) {
      auto status = WriteImage(disk_manager_, record.page_id, record.after_image);
      if (!status.ok()) return Status::IOError("committed redo failed: " + status.message());
    } else if (record.type == LogRecordType::PageFree) {
      auto status = DeallocateIdempotent(disk_manager_, record.page_id);
      if (!status.ok()) return Status::IOError("committed page free redo failed: " + status.message());
    }
  }

  // Undo every transaction without a durable terminal record, newest first.
  for (const auto &[txn_id, records] : by_transaction) {
    if (committed.find(txn_id) != committed.end() || terminal.find(txn_id) != terminal.end()) continue;
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
      const auto &record = **it;
      if (record.type == LogRecordType::PageUpdate) {
        auto status = WriteImage(disk_manager_, record.page_id, record.before_image);
        if (!status.ok()) return Status::IOError("uncommitted undo failed: " + status.message());
      } else if (record.type == LogRecordType::PageAllocate) {
        auto status = DeallocateIdempotent(disk_manager_, record.page_id);
        if (!status.ok()) return Status::IOError("uncommitted allocation undo failed: " + status.message());
      }
      // A PageFree is deferred and therefore needs no undo at crash time.
    }
  }
  return Status::Ok();
}

}  // namespace oursql
