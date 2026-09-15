#include "oursql/recovery/recovery_manager.h"

#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace oursql {

namespace {

struct RecoveryTransaction {
  // 恢复阶段只需记录每个事务最后一条日志及其终止状态。
  lsn_t last_lsn{kInvalidLsn};
  bool terminal{false};
  LogRecordType terminal_type{LogRecordType::Checkpoint};
  lsn_t terminal_lsn{kInvalidLsn};
};

Status WriteImage(DiskManager *disk_manager, page_id_t page_id,
                  const std::vector<std::byte> &image) {
  // 调用者：Redo/Undo；作用：通过 DiskManager 写回一张完整页面；返回：写入状态。
  // 恢复写整页必须仍经过 DiskManager，不能直接操作 .oursql 文件。
  if (disk_manager == nullptr || image.size() != Page::kSize) {
    return Status::InvalidArgument("recovery requires one full page image");
  }
  Page page;
  std::copy(image.begin(), image.end(), page.Data());
  return disk_manager->WritePage(page_id, page);
}

Status DeallocateIdempotent(DiskManager *disk_manager, page_id_t page_id) {
  // 调用者：Redo/Undo；作用：幂等释放物理页；返回：AlreadyExists 也视为成功。
  // 恢复可能重复执行同一释放动作，AlreadyExists 在这里视为幂等成功。
  if (disk_manager == nullptr) {
    return Status::InvalidArgument("recovery requires a DiskManager");
  }
  auto status = disk_manager->DeallocatePage(page_id);
  return status.code() == ErrorCode::AlreadyExists ? Status::Ok() : status;
}

Status RedoCompensation(DiskManager *disk_manager, const LogRecord &record) {
  // 调用者：Recover 的 Redo 阶段；作用：重放一条 CLR；返回：重放状态。
  // CLR 的 Redo 必须可重复：页面镜像重写，分配回收使用幂等释放。
  switch (record.compensation_type) {
    case LogRecordType::PageUpdate:
      return WriteImage(disk_manager, record.page_id, record.after_image);
    case LogRecordType::PageAllocate:
      return DeallocateIdempotent(disk_manager, record.page_id);
    case LogRecordType::PageFree:
      // PageFree is deferred until commit cleanup. A loser never applied it.
      return Status::Ok();
    default:
      return Status::InvalidArgument("recovery CLR target type is invalid");
  }
}

}  // namespace

Status RecoveryManager::Recover() {
  if (log_manager_ == nullptr || disk_manager_ == nullptr) {
    return Status::InvalidArgument("RecoveryManager requires LogManager and DiskManager");
  }
  // 第一步：读取并校验 WAL，恢复只使用完整、格式正确的记录。
  auto scan = log_manager_->Scan();
  if (!scan.ok()) return Status::IOError("WAL recovery scan failed: " + scan.status().message());

  // 第二步：按事务分析日志，区分已 Commit 的 winner 和没有终止记录的 loser。
  std::unordered_map<txn_id_t, RecoveryTransaction> transactions;
  std::unordered_map<lsn_t, const LogRecord *> records_by_lsn;
  for (const auto &record : scan.value()) {
    records_by_lsn.emplace(record.lsn, &record);
    if (record.txn_id == kInvalidTxnId) continue;
    auto &transaction = transactions[record.txn_id];
    transaction.last_lsn = std::max(transaction.last_lsn, record.lsn);
    if ((record.type == LogRecordType::Commit || record.type == LogRecordType::Abort) &&
        record.lsn >= transaction.terminal_lsn) {
      transaction.terminal = true;
      transaction.terminal_type = record.type;
      transaction.terminal_lsn = record.lsn;
    }
  }

  std::unordered_set<txn_id_t> winners;
  std::unordered_set<txn_id_t> losers;
  for (const auto &[txn_id, transaction] : transactions) {
    if (transaction.terminal && transaction.terminal_type == LogRecordType::Commit) {
      winners.insert(txn_id);
    } else if (!transaction.terminal) {
      losers.insert(txn_id);
    }
  }

  // 第三步 Redo：按全局 LSN 顺序重放 winner 的 PageUpdate/PageFree。
  // Redo is global-LSN ordered. A CLR is replayed for both winners and
  // unfinished losers: the latter is what makes recovery resumable when the
  // process stopped after logging a CLR but before writing its page image.
  for (const auto &record : scan.value()) {
    const bool is_winner = winners.find(record.txn_id) != winners.end();
    const bool is_loser = losers.find(record.txn_id) != losers.end();
    if (!is_winner && !(is_loser && record.type == LogRecordType::Compensation)) {
      continue;
    }
    Status status = Status::Ok();
    if (record.type == LogRecordType::PageUpdate) {
      status = WriteImage(disk_manager_, record.page_id, record.after_image);
    } else if (record.type == LogRecordType::PageFree) {
      status = DeallocateIdempotent(disk_manager_, record.page_id);
    } else if (record.type == LogRecordType::Compensation) {
      status = RedoCompensation(disk_manager_, record);
    }
    if (!status.ok()) {
      return Status::IOError("committed redo failed at LSN " +
                             std::to_string(record.lsn) + ": " + status.message());
    }
  }

  // 第四步 Undo：用最大 LSN 优先堆处理交错的 loser 日志。
  // Undo all losers with one global max-LSN heap. This handles interleaved
  // transaction logs and leaves a resumable CLR chain in the WAL.
  using PendingUndo = std::pair<lsn_t, txn_id_t>;
  std::priority_queue<PendingUndo> pending;
  std::unordered_map<txn_id_t, lsn_t> next_lsn;
  for (const auto txn_id : losers) {
    const auto last = transactions.at(txn_id).last_lsn;
    next_lsn[txn_id] = last;
    if (last != kInvalidLsn) pending.emplace(last, txn_id);
  }

  lsn_t recovery_last_lsn = kInvalidLsn;
  while (!pending.empty()) {
    // 每次取某个 loser 当前最大的 LSN，保证逆向处理顺序。
    const auto [current_lsn, txn_id] = pending.top();
    pending.pop();
    if (next_lsn[txn_id] != current_lsn) continue;

    const auto found = records_by_lsn.find(current_lsn);
    if (found == records_by_lsn.end() || found->second->txn_id != txn_id) {
      return Status::IOError("loser transaction WAL chain is broken at LSN " +
                             std::to_string(current_lsn));
    }
    const auto &record = *found->second;
    lsn_t following_lsn = record.prev_lsn;

    if (record.type == LogRecordType::Compensation) {
      // This action was completed by an earlier recovery attempt.
      following_lsn = record.undo_next_lsn;
    } else if (record.type == LogRecordType::PageUpdate ||
               record.type == LogRecordType::PageAllocate ||
               record.type == LogRecordType::PageFree) {
      // 先追加 CLR，再执行物理 Undo；这样崩溃后可根据 CLR 继续恢复。
      LogRecord compensation;
      compensation.type = LogRecordType::Compensation;
      compensation.txn_id = txn_id;
      compensation.prev_lsn = transactions.at(txn_id).last_lsn;
      compensation.page_id = record.page_id;
      compensation.undo_next_lsn = record.prev_lsn;
      compensation.compensation_type = record.type;
      if (record.type == LogRecordType::PageUpdate) {
        compensation.after_image = record.before_image;
      }

      auto compensation_lsn = log_manager_->Append(std::move(compensation));
      if (!compensation_lsn.ok()) {
        return Status::IOError("recovery CLR append failed for transaction " +
                               std::to_string(txn_id) + ": " +
                               compensation_lsn.status().message());
      }
      transactions.at(txn_id).last_lsn = compensation_lsn.value();
      recovery_last_lsn = std::max(recovery_last_lsn, compensation_lsn.value());

      // PageUpdate 恢复 before image；PageAllocate 撤销分配；PageFree 延迟释放无需反向操作。
      Status undo_status = Status::Ok();
      if (record.type == LogRecordType::PageUpdate) {
        undo_status = WriteImage(disk_manager_, record.page_id, record.before_image);
      } else if (record.type == LogRecordType::PageAllocate) {
        undo_status = DeallocateIdempotent(disk_manager_, record.page_id);
      }
      if (!undo_status.ok()) {
        return Status::IOError("loser undo failed at LSN " +
                               std::to_string(record.lsn) + ": " +
                               undo_status.message());
      }
    }

    next_lsn[txn_id] = following_lsn;
    if (following_lsn != kInvalidLsn) pending.emplace(following_lsn, txn_id);
  }

  // 第五步：所有 loser 的 Undo 完成后才追加 Abort，避免过早宣布恢复结束。
  // Mark a loser terminal only after all of its undo actions are logged and
  // applied. One flush groups all recovery CLRs and Abort records.
  for (const auto txn_id : losers) {
    LogRecord abort;
    abort.type = LogRecordType::Abort;
    abort.txn_id = txn_id;
    abort.prev_lsn = transactions.at(txn_id).last_lsn;
    auto abort_lsn = log_manager_->Append(std::move(abort));
    if (!abort_lsn.ok()) {
      return Status::IOError("recovery Abort append failed for transaction " +
                             std::to_string(txn_id) + ": " + abort_lsn.status().message());
    }
    recovery_last_lsn = std::max(recovery_last_lsn, abort_lsn.value());
  }

  // 一次性刷入本次恢复产生的 CLR 和 Abort，保证恢复结果持久化。
  const auto flush_target = std::max(recovery_last_lsn, log_manager_->GetAppendLsn());
  auto flush_status = log_manager_->Flush(flush_target);
  if (!flush_status.ok()) {
    return Status::IOError("recovery WAL flush failed: " + flush_status.message());
  }
  return Status::Ok();
}

}  // namespace oursql
