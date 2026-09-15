#include "oursql/storage/heap_table.h"

#include <utility>

namespace oursql {

// HeapTable 负责把“表”组织为 SlottedPage 单向链表：TableMetadata 保存首页，
// 每条记录在页内由 slot 定位，因此稳定地址 RID = (page_id, slot_id)。
// 行的字节表示交给 RowCodec，页内空间管理交给 SlottedPage，本类只协调跨页操作。

// 调用者：Catalog/Execution；作用：绑定缓存池和表元数据；返回：构造的堆表对象。
HeapTable::HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept
    : buffer_pool_(buffer_pool), metadata_(std::move(metadata)) {}

Result<std::optional<RowEntry>> HeapTable::ScanCursor::Next() {
  // 调用者：SeqScanExecutor/HeapTable::Scan；作用：沿数据页链返回下一条有效行；返回：RowEntry 或扫描结束。
  if (buffer_pool_ == nullptr) {
    return Result<std::optional<RowEntry>>(Status::InvalidArgument("HeapTable 扫描游标缺少 BufferPoolManager"));
  }
  while (true) {
    // 游标按“页链 -> 页内 slot”的顺序推进；仅在需要时取一页，形成惰性扫描。
    if (!page_guard_.has_value()) {
      if (current_page_ == INVALID_PAGE_ID) {
        return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
      }
      if (!page_loaded_ && (current_page_ == 0 || !visited_pages_.insert(current_page_).second)) {
        return Result<std::optional<RowEntry>>(
            Status::InvalidArgument("数据页链表损坏或成环: " + std::to_string(current_page_)));
      }
      // page_guard_ 在读取当前记录期间固定页面，避免缓冲池淘汰其底层帧。
      auto page_result = buffer_pool_->FetchPage(current_page_);
      if (!page_result.ok()) return Result<std::optional<RowEntry>>(page_result.status());
      page_guard_.emplace(std::move(page_result.value()));
      auto valid = SlottedPage::Validate(*page_guard_);
      if (!valid.ok()) {
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(valid);
      }
      auto count = SlottedPage::SlotCount(*page_guard_);
      if (!count.ok()) {
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(count.status());
      }
      slot_count_ = count.value();
      page_loaded_ = true;
    }

    if (page_guard_.has_value() && !page_loaded_) {
      auto valid = SlottedPage::Validate(*page_guard_);
      if (!valid.ok()) {
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(valid);
      }
      auto count = SlottedPage::SlotCount(*page_guard_);
      if (!count.ok()) {
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(count.status());
      }
      slot_count_ = count.value();
      page_loaded_ = true;
    }

    while (next_slot_ < slot_count_) {
      const auto slot_id = static_cast<slot_id_t>(next_slot_++);
      auto record = SlottedPage::GetRecord(*page_guard_, slot_id);
      if (!record.ok()) {
        // 删除只留下不可见 slot；扫描应跳过空洞，而不是提前结束。
        if (record.status().code() == ErrorCode::NotFound) continue;
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(record.status());
      }
      auto row = RowCodec::Decode(metadata_.schema, record.value());
      if (!row.ok()) {
        page_guard_.reset();
        return Result<std::optional<RowEntry>>(row.status());
      }
      RowEntry entry{RID{current_page_, slot_id}, std::move(row.value())};
      // 返回前释放页钉住状态，使长期存活的游标不会长期占用缓冲池帧。
      page_guard_.reset();
      return Result<std::optional<RowEntry>>(std::optional<RowEntry>(std::move(entry)));
    }

    // 当前页耗尽后沿 next_page_id 前进，并从新页的 slot 0 重新开始。
    auto next = SlottedPage::NextPageId(*page_guard_);
    if (!next.ok()) return Result<std::optional<RowEntry>>(next.status());
    if (next.value() != INVALID_PAGE_ID) {
      // 先 pin 下一页，再释放当前页，防止链交接期间目标页被淘汰或回收。
      auto next_result = buffer_pool_->FetchPage(next.value());
      if (!next_result.ok()) return Result<std::optional<RowEntry>>(next_result.status());
      auto next_guard = std::move(next_result.value());
      page_guard_.reset();
      page_guard_.emplace(std::move(next_guard));
    } else {
      page_guard_.reset();
    }
    current_page_ = next.value();
    page_loaded_ = false;
    next_slot_ = 0;
  }
}

Result<HeapTable::ScanCursor> HeapTable::BeginScan() {
  // 调用者：SeqScanExecutor；作用：创建惰性扫描游标，不立即复制整张表；返回：游标或元数据错误。
  if (buffer_pool_ == nullptr) {
    return Result<ScanCursor>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  }
  if (metadata_.first_data_page_id == INVALID_PAGE_ID || metadata_.first_data_page_id == 0) {
    return Result<ScanCursor>(Status::InvalidArgument("HeapTable 缺少首数据页"));
  }
  return Result<ScanCursor>(ScanCursor(buffer_pool_, metadata_));
}

Result<RID> HeapTable::InsertRow(const Row &row) {
  // 调用者：非事务 InsertExecutor；作用：转发到无事务参数的插入路径；返回：新 RID。
  return InsertRow(row, nullptr);
}

Result<RID> HeapTable::InsertRow(const Row &row, Transaction *transaction) {
  // 调用者：InsertExecutor；作用：编码并把一行插入数据页链；返回：新行 RID 或存储错误。
  if (buffer_pool_ == nullptr) return Result<RID>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  if (metadata_.first_data_page_id == INVALID_PAGE_ID || metadata_.first_data_page_id == 0) {
    return Result<RID>(Status::InvalidArgument("HeapTable 缺少首数据页"));
  }
  auto encoded = RowCodec::Encode(metadata_.schema, row);
  if (!encoded.ok()) return Result<RID>(encoded.status());
  if (encoded.value().size() > SlottedPage::kMaxRecordSize) {
    return Result<RID>(Status::RecordTooLarge(
        "HeapTable 单行超过页面上限: " + std::to_string(SlottedPage::kMaxRecordSize)));
  }

  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  std::optional<WritePageGuard> current_guard;
  while (true) {
    if (current == 0 || current == INVALID_PAGE_ID || !visited.insert(current).second) {
      return Result<RID>(Status::InvalidArgument("数据页链表损坏或成环"));
    }
    if (!current_guard.has_value()) {
      auto page_result = buffer_pool_->FetchPageWrite(current, transaction);
      if (!page_result.ok()) return Result<RID>(page_result.status());
      current_guard.emplace(std::move(page_result.value()));
    }

    auto slot = SlottedPage::InsertRecord(*current_guard, encoded.value());
    if (slot.ok()) return Result<RID>(RID{current, slot.value()});
    if (slot.status().code() != ErrorCode::OutOfSpace) {
      return Result<RID>(slot.status());
    }
    auto next_result = SlottedPage::NextPageId(*current_guard);
    if (!next_result.ok()) return Result<RID>(next_result.status());
    const page_id_t next = next_result.value();
    if (next != INVALID_PAGE_ID) {
      auto next_page_result = buffer_pool_->FetchPageWrite(next, transaction);
      if (!next_page_result.ok()) return Result<RID>(next_page_result.status());
      auto next_guard = std::move(next_page_result.value());
      const auto release_status = current_guard->Release();
      current_guard.reset();
      if (!release_status.ok()) return Result<RID>(release_status);
      current_guard.emplace(std::move(next_guard));
      current = next;
      continue;
    }

    // 尾页仍由当前 WriteGuard 保护，创建和链接新页期间不会出现两个后继。
    auto new_page_result = buffer_pool_->NewPage(transaction);
    if (!new_page_result.ok()) return Result<RID>(new_page_result.status());
    auto new_page = std::move(new_page_result.value());
    const page_id_t new_page_id = new_page.PageId();
    auto init_status = SlottedPage::Initialize(new_page);
    if (!init_status.ok()) {
      (void)new_page.Release();
      (void)buffer_pool_->DeletePage(new_page_id, transaction);
      return Result<RID>(init_status);
    }
    auto new_slot = SlottedPage::InsertRecord(new_page, encoded.value());
    if (!new_slot.ok()) {
      (void)new_page.Release();
      (void)buffer_pool_->DeletePage(new_page_id, transaction);
      return Result<RID>(new_slot.status());
    }
    auto link_status = SlottedPage::SetNextPageId(*current_guard, new_page_id);
    if (!link_status.ok()) {
      (void)new_page.Release();
      (void)buffer_pool_->DeletePage(new_page_id, transaction);
      return Result<RID>(link_status);
    }
    const auto release_status = current_guard->Release();
    current_guard.reset();
    if (!release_status.ok()) return Result<RID>(release_status);
    return Result<RID>(RID{new_page_id, new_slot.value()});
  }
  }
Status HeapTable::ValidateRidPage(page_id_t page_id) {
  // 调用者：GetRow/DeleteRow；作用：确认 RID 指向该表的数据页；返回：合法性状态。
  if (page_id == 0 || page_id == INVALID_PAGE_ID) {
    return Status::NotFound("RID 页面编号非法: " + std::to_string(page_id));
  }
  if (metadata_.first_data_page_id == 0 || metadata_.first_data_page_id == INVALID_PAGE_ID) {
    return Status::InvalidArgument("HeapTable 缺少首数据页");
  }

  // page_id 全局存在并不代表属于本表；沿本表页链验证所有权，防止跨表 RID 访问。
  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  std::optional<ReadPageGuard> page_guard;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || !visited.insert(current).second) {
      return Status::InvalidArgument("数据页链表损坏或成环");
    }
    if (!page_guard.has_value()) {
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      page_guard.emplace(std::move(page_result.value()));
    }
    auto valid = SlottedPage::Validate(*page_guard);
    if (!valid.ok()) return valid;
    auto next = SlottedPage::NextPageId(*page_guard);
    if (!next.ok()) return next.status();
    if (current == page_id) return Status::Ok();
    if (next.value() != INVALID_PAGE_ID) {
      auto next_result = buffer_pool_->FetchPage(next.value());
      if (!next_result.ok()) return next_result.status();
      auto next_guard = std::move(next_result.value());
      page_guard.reset();
      page_guard.emplace(std::move(next_guard));
    } else {
      page_guard.reset();
    }
    current = next.value();
  }
  return Status::NotFound("RID 页面不属于当前表: " + std::to_string(page_id));
}

Result<Row> HeapTable::GetRow(const RID &rid) {
  // 调用者：查询执行器；作用：通过 RID 读取并解码一行；返回：Row 或无效 RID/损坏错误。
  if (buffer_pool_ == nullptr) return Result<Row>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  auto ownership = ValidateRidPage(rid.page_id);
  if (!ownership.ok()) return Result<Row>(ownership);
  // 读路径使用只读页守卫，取得原始记录后再依据当前表 Schema 解码。
  auto page_result = buffer_pool_->FetchPage(rid.page_id);
  if (!page_result.ok()) return Result<Row>(page_result.status());
  auto page = std::move(page_result.value());
  auto record = SlottedPage::GetRecord(page, rid.slot_id);
  if (!record.ok()) return Result<Row>(record.status());
  return RowCodec::Decode(metadata_.schema, record.value());
}

Status HeapTable::DeleteRow(const RID &rid) {
  // 调用者：非事务 DeleteExecutor；作用：转发到无事务删除路径；返回：删除状态。
  return DeleteRow(rid, nullptr);
}

Status HeapTable::DeleteRow(const RID &rid, Transaction *transaction) {
  // 调用者：DeleteExecutor/事务回滚；作用：标记 RID 对应槽位删除；返回：删除状态。
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("HeapTable 缺少 BufferPoolManager");
  auto ownership = ValidateRidPage(rid.page_id);
  if (!ownership.ok()) return ownership;

  auto target_result = buffer_pool_->FetchPageWrite(rid.page_id, transaction);
  if (!target_result.ok()) return target_result.status();
  auto target_page = std::move(target_result.value());
  auto next_result = SlottedPage::NextPageId(target_page);
  if (!next_result.ok()) return next_result.status();
  auto delete_status = SlottedPage::DeleteRecord(target_page, rid.slot_id);
  if (!delete_status.ok()) return delete_status;
  const auto release_status = target_page.Release();
  if (!release_status.ok()) return release_status;

  if (rid.page_id == metadata_.first_data_page_id) return Status::Ok();

  // 先按链顺序找到前驱；跨页交接时先 pin 下一页，再释放当前页。
  page_id_t predecessor = INVALID_PAGE_ID;
  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  std::optional<ReadPageGuard> cursor;
  while (current != INVALID_PAGE_ID && current != rid.page_id) {
    if (current == 0 || !visited.insert(current).second) {
      return Status::InvalidArgument("数据页链表损坏或成环");
    }
    if (!cursor.has_value()) {
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      cursor.emplace(std::move(page_result.value()));
    }
    auto next = SlottedPage::NextPageId(*cursor);
    if (!next.ok()) return next.status();
    if (next.value() == rid.page_id) {
      predecessor = current;
      break;
    }
    if (next.value() == INVALID_PAGE_ID) break;
    auto next_result_for_cursor = buffer_pool_->FetchPage(next.value());
    if (!next_result_for_cursor.ok()) return next_result_for_cursor.status();
    auto next_cursor = std::move(next_result_for_cursor.value());
    cursor.reset();
    cursor.emplace(std::move(next_cursor));
    current = next.value();
  }
  cursor.reset();
  if (predecessor == INVALID_PAGE_ID) {
    return Status::InternalError("已确认归属的数据页找不到前驱: " + std::to_string(rid.page_id));
  }

  auto predecessor_result = buffer_pool_->FetchPageWrite(predecessor, transaction);
  if (!predecessor_result.ok()) return predecessor_result.status();
  auto predecessor_page = std::move(predecessor_result.value());
  auto predecessor_next = SlottedPage::NextPageId(predecessor_page);
  if (!predecessor_next.ok()) return predecessor_next.status();
  if (predecessor_next.value() != rid.page_id) {
    return Status::InvalidArgument("删除空数据页时发现链表已变化");
  }

  auto target_again_result = buffer_pool_->FetchPageWrite(rid.page_id, transaction);
  if (!target_again_result.ok()) return target_again_result.status();
  auto target_again = std::move(target_again_result.value());
  auto target_next_again = SlottedPage::NextPageId(target_again);
  if (!target_next_again.ok()) return target_next_again.status();
  auto target_count = SlottedPage::SlotCount(target_again);
  if (!target_count.ok()) return target_count.status();
  for (std::uint32_t slot = 0; slot < target_count.value(); ++slot) {
    auto record = SlottedPage::GetRecord(target_again, static_cast<slot_id_t>(slot));
    if (record.ok()) {
      return Status::Ok();
    }
    if (record.status().code() != ErrorCode::NotFound) return record.status();
  }

  auto reserve_status = buffer_pool_->BeginPageDeletion(rid.page_id, 1);
  if (!reserve_status.ok()) return reserve_status;
  auto link_status = SlottedPage::SetNextPageId(predecessor_page, target_next_again.value());
  if (!link_status.ok()) {
    (void)target_again.Release();
    (void)predecessor_page.Release();
    (void)buffer_pool_->CancelPageDeletion(rid.page_id);
    return link_status;
  }
  const auto target_release = target_again.Release();
  const auto predecessor_release = predecessor_page.Release();
  if (!target_release.ok() || !predecessor_release.ok()) {
    const auto cancel_status = buffer_pool_->CancelPageDeletion(rid.page_id);
    if (!cancel_status.ok() && cancel_status.code() != ErrorCode::NotFound) {
      return Status::IOError("取消数据页删除预留失败: " + cancel_status.message());
    }
    return !target_release.ok() ? target_release : predecessor_release;
  }
  if (transaction != nullptr) {
    auto log_status = buffer_pool_->DeletePage(rid.page_id, transaction);
    if (!log_status.ok()) {
      (void)buffer_pool_->CancelPageDeletion(rid.page_id);
      return log_status;
    }
    return Status::Ok();
  }
  return buffer_pool_->FinalizePageDeletion(rid.page_id);
}
Result<std::vector<RowEntry>> HeapTable::Scan() {
  // 调用者：兼容接口/测试；作用：消费惰性游标并物化有效行；返回：所有 RowEntry 或错误。
  // 便利接口：消费惰性游标并物化整表；执行 SELECT 时优先直接使用 BeginScan。
  std::vector<RowEntry> rows;
  auto cursor = BeginScan();
  if (!cursor.ok()) return Result<std::vector<RowEntry>>(cursor.status());
  while (true) {
    auto entry = cursor.value().Next();
    if (!entry.ok()) return Result<std::vector<RowEntry>>(entry.status());
    if (!entry.value().has_value()) break;
    rows.push_back(std::move(entry.value().value()));
  }
  return Result<std::vector<RowEntry>>(std::move(rows));
}

Status HeapTable::Destroy() {
  // 调用者：表生命周期管理；作用：删除该表数据页链并释放底层页面；返回：销毁状态。
  if (buffer_pool_ == nullptr) {
    return Status::InvalidArgument("HeapTable missing BufferPoolManager");
  }
  if (metadata_.first_data_page_id == 0 ||
      metadata_.first_data_page_id == INVALID_PAGE_ID) {
    return Status::InvalidArgument("HeapTable has no valid first data page");
  }

  // Phase one validates the complete chain before any page is released.
  std::vector<page_id_t> pages;
  std::unordered_set<page_id_t> visited;
  page_id_t current = metadata_.first_data_page_id;
  std::optional<ReadPageGuard> page_guard;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || !visited.insert(current).second) {
      return Status::InvalidArgument("HeapTable data page chain is invalid or cyclic");
    }
    if (!page_guard.has_value()) {
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      page_guard.emplace(std::move(page_result.value()));
    }
    auto valid = SlottedPage::Validate(*page_guard);
    if (!valid.ok()) return valid;
    auto next_result = SlottedPage::NextPageId(*page_guard);
    if (!next_result.ok()) return next_result.status();
    const page_id_t next = next_result.value();
    pages.push_back(current);
    if (next != INVALID_PAGE_ID) {
      auto next_page_result = buffer_pool_->FetchPage(next);
      if (!next_page_result.ok()) return next_page_result.status();
      auto next_guard = std::move(next_page_result.value());
      page_guard.reset();
      page_guard.emplace(std::move(next_guard));
    } else {
      page_guard.reset();
    }
    current = next;
  }

  // DeletePages checks every pin before changing any page. It is still not
  // transactional: without WAL, a later I/O error can release only a prefix.
  auto delete_status = buffer_pool_->DeletePages(pages);
  if (!delete_status.ok()) return delete_status;
  metadata_.first_data_page_id = INVALID_PAGE_ID;
  return Status::Ok();
}

}  // namespace oursql
