#include "oursql/storage/heap_table.h"

#include <utility>

namespace oursql {

// HeapTable 负责把“表”组织为 SlottedPage 单向链表：TableMetadata 保存首页，
// 每条记录在页内由 slot 定位，因此稳定地址 RID = (page_id, slot_id)。
// 行的字节表示交给 RowCodec，页内空间管理交给 SlottedPage，本类只协调跨页操作。

HeapTable::HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept
    : buffer_pool_(buffer_pool), metadata_(std::move(metadata)) {}

Result<std::optional<RowEntry>> HeapTable::ScanCursor::Next() {
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
    page_guard_.reset();
    if (!next.ok()) return Result<std::optional<RowEntry>>(next.status());
    current_page_ = next.value();
    page_loaded_ = false;
    next_slot_ = 0;
  }
}

Result<HeapTable::ScanCursor> HeapTable::BeginScan() {
  if (buffer_pool_ == nullptr) {
    return Result<ScanCursor>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  }
  if (metadata_.first_data_page_id == INVALID_PAGE_ID || metadata_.first_data_page_id == 0) {
    return Result<ScanCursor>(Status::InvalidArgument("HeapTable 缺少首数据页"));
  }
  return Result<ScanCursor>(ScanCursor(buffer_pool_, metadata_));
}

Result<RID> HeapTable::InsertRow(const Row &row) {
  if (buffer_pool_ == nullptr) return Result<RID>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  if (metadata_.first_data_page_id == INVALID_PAGE_ID || metadata_.first_data_page_id == 0) {
    return Result<RID>(Status::InvalidArgument("HeapTable 缺少首数据页"));
  }
  // 先完成类型/长度校验和序列化，失败时不会触碰任何数据页。
  auto encoded = RowCodec::Encode(metadata_.schema, row);
  if (!encoded.ok()) return Result<RID>(encoded.status());
  if (encoded.value().size() > SlottedPage::kMaxRecordSize) {
    return Result<RID>(Status::RecordTooLarge(
        "HeapTable 单行超过页面上限: " + std::to_string(SlottedPage::kMaxRecordSize)));
  }

  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  // 采用 first-fit：从首页起寻找第一个能容纳记录的页面。
  while (true) {
    if (current == 0 || current == INVALID_PAGE_ID || !visited.insert(current).second) {
      return Result<RID>(Status::InvalidArgument("数据页链表损坏或成环"));
    }
    auto page_result = buffer_pool_->FetchPageWrite(current);
    if (!page_result.ok()) return Result<RID>(page_result.status());
    page_id_t next = INVALID_PAGE_ID;
    bool inserted = false;
    slot_id_t slot_id = 0;
    {
      auto page = std::move(page_result.value());
      auto slot = SlottedPage::InsertRecord(page, encoded.value());
      if (slot.ok()) {
        inserted = true;
        slot_id = slot.value();
      } else if (slot.status().code() != ErrorCode::OutOfSpace) {
        return Result<RID>(slot.status());
      } else {
        // 页满不是错误：若已有后继页则继续尝试，否则在链尾扩容。
        auto next_result = SlottedPage::NextPageId(page);
        if (!next_result.ok()) return Result<RID>(next_result.status());
        next = next_result.value();
      }
    }
    if (inserted) return Result<RID>(RID{current, slot_id});
    if (next != INVALID_PAGE_ID) {
      current = next;
      continue;
    }

    // 链尾无空间：先创建并初始化新页，再把旧尾页指向它。
    auto new_page_result = buffer_pool_->NewPageGuarded();
    if (!new_page_result.ok()) return Result<RID>(new_page_result.status());
    page_id_t new_page_id = INVALID_PAGE_ID;
    Status init_status = Status::Ok();
    {
      auto new_page = std::move(new_page_result.value());
      new_page_id = new_page.PageId();
      init_status = SlottedPage::Initialize(new_page);
    }
    if (!init_status.ok()) {
      (void)buffer_pool_->DeletePage(new_page_id);
      return Result<RID>(init_status);
    }

    auto link_result = buffer_pool_->FetchPageWrite(current);
    if (!link_result.ok()) {
      (void)buffer_pool_->DeletePage(new_page_id);
      return Result<RID>(link_result.status());
    }
    {
      auto link_page = std::move(link_result.value());
      auto link_status = SlottedPage::SetNextPageId(link_page, new_page_id);
      if (!link_status.ok()) {
        (void)buffer_pool_->DeletePage(new_page_id);
        return Result<RID>(link_status);
      }
    }
    // 下一轮会在刚创建的空页执行正常插入，统一 RID 的生成路径。
    current = new_page_id;
  }
}

Status HeapTable::ValidateRidPage(page_id_t page_id) {
  if (page_id == 0 || page_id == INVALID_PAGE_ID) {
    return Status::NotFound("RID 页面编号非法: " + std::to_string(page_id));
  }
  if (metadata_.first_data_page_id == 0 || metadata_.first_data_page_id == INVALID_PAGE_ID) {
    return Status::InvalidArgument("HeapTable 缺少首数据页");
  }

  // page_id 全局存在并不代表属于本表；沿本表页链验证所有权，防止跨表 RID 访问。
  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || !visited.insert(current).second) {
      return Status::InvalidArgument("数据页链表损坏或成环");
    }
    auto page_result = buffer_pool_->FetchPage(current);
    if (!page_result.ok()) return page_result.status();
    auto page = std::move(page_result.value());
    auto valid = SlottedPage::Validate(page);
    if (!valid.ok()) return valid;
    auto next = SlottedPage::NextPageId(page);
    if (!next.ok()) return next.status();
    if (current == page_id) return Status::Ok();
    current = next.value();
  }
  return Status::NotFound("RID 页面不属于当前表: " + std::to_string(page_id));
}

Result<Row> HeapTable::GetRow(const RID &rid) {
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
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("HeapTable 缺少 BufferPoolManager");
  auto ownership = ValidateRidPage(rid.page_id);
  if (!ownership.ok()) return ownership;
  // 删除仅修改 slot/page 元数据；页面仍留在表链中，后续插入可复用其空间。
  auto page_result = buffer_pool_->FetchPageWrite(rid.page_id);
  if (!page_result.ok()) return page_result.status();
  auto page = std::move(page_result.value());
  return SlottedPage::DeleteRecord(page, rid.slot_id);
}

Result<std::vector<RowEntry>> HeapTable::Scan() {
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

}  // namespace oursql
