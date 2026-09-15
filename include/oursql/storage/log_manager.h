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

// WAL 的每条记录都带 LSN；同一事务通过 prev_lsn 串成自己的日志链。
// PageUpdate 保存修改前/后的整页镜像，供 Undo 和 Redo 使用。
// A decoded WAL record. PageUpdate uses full before/after page images.
struct LogRecord {
  LogRecordType type{LogRecordType::Checkpoint};
  lsn_t lsn{kInvalidLsn};  // 全局递增的日志序号。
  txn_id_t txn_id{kInvalidTxnId};
  lsn_t prev_lsn{kInvalidLsn};  // 同一事务上一条日志的 LSN。
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
  // 调用者：数据库引擎；作用：创建尚未打开 WAL 的管理器；返回：空对象。
  LogManager() = default;
  // 调用者：对象生命周期结束；作用：关闭 WAL 并释放文件资源；返回：无。
  ~LogManager();

  LogManager(const LogManager &) = delete;
  LogManager &operator=(const LogManager &) = delete;

  // 调用者：数据库启动流程；作用：打开对应的 .wal，校验记录并修剪断裂尾部；返回：打开状态。
  [[nodiscard]] Status Open(const std::filesystem::path &database_path);
  // 调用者：数据库关闭流程；作用：刷出并关闭 WAL 文件；返回：关闭状态。
  [[nodiscard]] Status Close();

  // 调用者：事务/恢复层；作用：编码并追加一条 WAL；返回：新分配的 LSN。
  [[nodiscard]] Result<lsn_t> Append(LogRecord record);
  // 调用者：BufferPool/事务提交；作用：确保 target_lsn 前的 WAL 已落盘；返回：刷盘状态。
  [[nodiscard]] Status Flush(lsn_t target_lsn);
  // 调用者：恢复/Undo；作用：按追加顺序读取并校验所有 WAL；返回：解码后的记录列表。
  [[nodiscard]] Result<std::vector<LogRecord>> Scan() const;
  // 调用者：恢复/启动流程；作用：扫描 WAL 计算下一个未使用事务号；返回：事务号或溢出错误。
  [[nodiscard]] Result<txn_id_t> GetNextTransactionIdSeed() const;
  // 调用者：完成静态 checkpoint 的协调器；作用：清空已不再需要的 WAL；返回：截断状态。
  [[nodiscard]] Status Truncate();

  // 调用者：故障测试；作用：让下一次 WAL Flush 返回 I/O 错误；返回：无。
  void FailNextFlushForTesting() noexcept;

  // 调用者：事务/诊断；作用：读取最后分配的 LSN；返回：LSN 快照。
  [[nodiscard]] lsn_t GetAppendLsn() const;
  // 调用者：提交/诊断；作用：读取已经物理刷盘的 LSN；返回：LSN 快照。
  [[nodiscard]] lsn_t GetDurableLsn() const;
  // 调用者：诊断/恢复；作用：取得当前 WAL 路径；返回：路径引用，生命周期属于 LogManager。
  [[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

 private:
  static constexpr std::uint32_t kMagic = 0x314C4157U;  // WAL1
  static constexpr std::uint16_t kVersion = 1;
  static constexpr std::size_t kHeaderSize = 48;

  // 以下辅助函数要求调用者已持有 mutex_；file_mutex_ 负责保护 fstream。
  // 调用者：Open/Append/Scan/Flush；作用：检查 WAL 已打开；返回：可用状态。
  [[nodiscard]] Status EnsureOpenUnlocked() const;
  // 调用者：Open 或 Scan；作用：读取并校验日志，按需修剪损坏尾部；返回：记录列表。
  [[nodiscard]] Result<std::vector<LogRecord>> ScanUnlocked(bool repair_tail) const;
  // 调用者：Append；作用：把结构化 LogRecord 编成固定头加 payload；返回：字节串或错误。
  [[nodiscard]] static Result<std::vector<std::byte>> Encode(const LogRecord &record,
                                                              lsn_t lsn);
  // 调用者：Scan；作用：校验一条 WAL 字节串并还原结构化记录；返回：LogRecord 或错误。
  [[nodiscard]] static Result<LogRecord> Decode(const std::byte *data,
                                                 std::size_t length);

  std::filesystem::path path_;
  mutable std::fstream file_;
  // 保护 WAL 的逻辑状态：LSN、flush 状态、打开状态和错误结果。
  mutable std::mutex mutex_;
  // 保护 fstream 本身；物理 flush/close/write 不能与其他文件操作交错。
  mutable std::mutex file_mutex_;
  // 串行化 Append、Open、Close 的生命周期操作，保证日志追加顺序。
  mutable std::mutex append_mutex_;
  // Flush 等待者通过它等待正在进行的刷 WAL 操作结束。
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
