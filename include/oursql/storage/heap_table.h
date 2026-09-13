#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/storage/row_codec.h"
#include "oursql/storage/storage.h"

#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oursql {

using RowEntry = std::pair<RID, Row>;

class HeapTable {
 public:
  // 调用者：执行器；作用：绑定一张已登记的堆表和缓冲池；返回：非拥有表对象。
  HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept;

  class ScanCursor {
   public:
    ScanCursor() = default;
    ~ScanCursor() = default;
    ScanCursor(const ScanCursor &) = delete;
    ScanCursor &operator=(const ScanCursor &) = delete;
    ScanCursor(ScanCursor &&) noexcept = default;
    ScanCursor &operator=(ScanCursor &&) noexcept = default;

    // 调用者：SeqScanExecutor；作用：逐行读取下一条有效记录；返回：行地址与行，结束时返回空值。
    [[nodiscard]] Result<std::optional<RowEntry>> Next();

   private:
    friend class HeapTable;
    ScanCursor(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept
        : buffer_pool_(buffer_pool), metadata_(std::move(metadata)),
          current_page_(metadata_.first_data_page_id) {}

    BufferPoolManager *buffer_pool_{nullptr};
    TableMetadata metadata_;
    page_id_t current_page_{INVALID_PAGE_ID};
    std::uint32_t next_slot_{0};
    std::optional<ReadPageGuard> page_guard_;
    bool page_loaded_{false};
    std::uint32_t slot_count_{0};
    std::unordered_set<page_id_t> visited_pages_;
  };

  // 调用者：SeqScanExecutor 或测试；作用：创建按页面链惰性读取的游标；返回：游标或错误。
  [[nodiscard]] Result<ScanCursor> BeginScan();

  // 调用者：插入执行器；作用：沿数据页链插入一行；返回：新行 RID 或错误。
  [[nodiscard]] Result<RID> InsertRow(const Row &row);
  [[nodiscard]] Result<RID> InsertRow(const Row &row, Transaction *transaction);
  // 调用者：点查执行器；作用：按 RID 获取并解码一行；返回：行或错误。
  [[nodiscard]] Result<Row> GetRow(const RID &rid);
  // 调用者：删除执行器；作用：按 RID 删除一行；返回：操作状态。
  [[nodiscard]] Status DeleteRow(const RID &rid);
  [[nodiscard]] Status DeleteRow(const RID &rid, Transaction *transaction);
  // 调用者：扫描执行器；作用：沿数据页链返回所有有效行；返回：RID 与行序列或错误。
  [[nodiscard]] Result<std::vector<RowEntry>> Scan();
  // 调用者：DROP TABLE 编排层；作用：校验并释放整条数据页链；返回：成功或首个存储错误。
  [[nodiscard]] Status Destroy();

 private:
  // 调用者：GetRow/DeleteRow；作用：确认 RID 页面属于当前表；返回：归属状态或链损坏错误。
  [[nodiscard]] Status ValidateRidPage(page_id_t page_id);

  BufferPoolManager *buffer_pool_{nullptr};
  TableMetadata metadata_;
};

}  // namespace oursql
