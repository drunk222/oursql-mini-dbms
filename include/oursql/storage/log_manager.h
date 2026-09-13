#pragma once

#include "oursql/common/status.h"
#include "oursql/common/types.h"
#include "oursql/transaction/transaction.h"

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace oursql {

enum class LogRecordType : std::uint16_t {
  Begin = 1,
  PageUpdate = 2,
  PageAllocate = 3,
  PageFree = 4,
  Commit = 5,
  Abort = 6,
  Checkpoint = 7,
  Compensation = 8,
};

// A decoded WAL record. PageUpdate uses full before/after page images.
struct LogRecord {
  LogRecordType type{LogRecordType::Checkpoint};
  lsn_t lsn{kInvalidLsn};
  txn_id_t txn_id{kInvalidTxnId};
  lsn_t prev_lsn{kInvalidLsn};
  page_id_t page_id{INVALID_PAGE_ID};
  // Compensation records point to the next original record to undo. When
  // present, after_image is the page image produced by that undo.
  lsn_t undo_next_lsn{kInvalidLsn};
  LogRecordType compensation_type{LogRecordType::Checkpoint};
  std::vector<std::byte> before_image;
  std::vector<std::byte> after_image;
};

class LogManager {
 public:
  LogManager() = default;
  ~LogManager();

  LogManager(const LogManager &) = delete;
  LogManager &operator=(const LogManager &) = delete;

  // Opens <database>.wal, validating complete records and trimming a torn tail.
  [[nodiscard]] Status Open(const std::filesystem::path &database_path);
  [[nodiscard]] Status Close();

  // Appends one encoded record and returns its newly assigned LSN.
  [[nodiscard]] Result<lsn_t> Append(LogRecord record);
  // Flushes WAL bytes through target_lsn before data pages are written.
  [[nodiscard]] Status Flush(lsn_t target_lsn);
  // Reads all valid records in append order.
  [[nodiscard]] Result<std::vector<LogRecord>> Scan() const;
  // Returns the first transaction id not present in the WAL, or overflow error.
  [[nodiscard]] Result<txn_id_t> GetNextTransactionIdSeed() const;
  // Removes WAL after a completed static checkpoint.
  [[nodiscard]] Status Truncate();

  // Tests only: makes the next WAL Flush fail without changing normal behavior.
  void FailNextFlushForTesting() noexcept;

  [[nodiscard]] lsn_t GetAppendLsn() const;
  [[nodiscard]] lsn_t GetDurableLsn() const;
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

 private:
  static constexpr std::uint32_t kMagic = 0x314C4157U;  // WAL1
  static constexpr std::uint16_t kVersion = 1;
  static constexpr std::size_t kHeaderSize = 48;

  [[nodiscard]] Status EnsureOpenUnlocked() const;
  [[nodiscard]] Result<std::vector<LogRecord>> ScanUnlocked(bool repair_tail) const;
  [[nodiscard]] static Result<std::vector<std::byte>> Encode(const LogRecord &record,
                                                              lsn_t lsn);
  [[nodiscard]] static Result<LogRecord> Decode(const std::byte *data,
                                                std::size_t length);

  std::filesystem::path path_;
  mutable std::fstream file_;
  mutable std::mutex mutex_;
  mutable std::mutex file_mutex_;
  mutable std::mutex append_mutex_;
  std::condition_variable flush_cv_;
  bool open_{false};
  bool fail_next_flush_for_testing_{false};
  bool flush_in_progress_{false};
  lsn_t flush_target_lsn_{kInvalidLsn};
  std::uint64_t flush_generation_{0};
  Status last_flush_status_{Status::Ok()};
  lsn_t append_lsn_{kInvalidLsn};
  lsn_t durable_lsn_{kInvalidLsn};
};

}  // namespace oursql
