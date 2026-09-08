#include "oursql/storage/heap_table.h"

#include <utility>

namespace oursql {

HeapTable::HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept
    : buffer_pool_(buffer_pool), metadata_(std::move(metadata)) {}

Result<std::optional<RowEntry>> HeapTable::ScanCursor::Next() {
  if (buffer_pool_ == nullptr) {
    return Result<std::optional<RowEntry>>(Status::InvalidArgument("HeapTable 扫描游标缺少 BufferPoolManager"));
  }
  while (true) {
    if (!page_guard_.has_value()) {
      if (current_page_ == INVALID_PAGE_ID) {
        return Result<std::optional<RowEntry>>(std::optional<RowEntry>{});
      }
      if (!page_loaded_ && (current_page_ == 0 || !visited_pages_.insert(current_page_).second)) {
        return Result<std::optional<RowEntry>>(
            Status::InvalidArgument("数据页链表损坏或成环: " + std::to_string(current_page_)));
      }
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
      page_guard_.reset();
      return Result<std::optional<RowEntry>>(std::optional<RowEntry>(std::move(entry)));
    }

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
  auto encoded = RowCodec::Encode(metadata_.schema, row);
  if (!encoded.ok()) return Result<RID>(encoded.status());

  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
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
    current = new_page_id;
  }
}

Result<Row> HeapTable::GetRow(const RID &rid) {
  if (buffer_pool_ == nullptr) return Result<Row>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  auto page_result = buffer_pool_->FetchPage(rid.page_id);
  if (!page_result.ok()) return Result<Row>(page_result.status());
  auto page = std::move(page_result.value());
  auto record = SlottedPage::GetRecord(page, rid.slot_id);
  if (!record.ok()) return Result<Row>(record.status());
  return RowCodec::Decode(metadata_.schema, record.value());
}

Status HeapTable::DeleteRow(const RID &rid) {
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("HeapTable 缺少 BufferPoolManager");
  auto page_result = buffer_pool_->FetchPageWrite(rid.page_id);
  if (!page_result.ok()) return page_result.status();
  auto page = std::move(page_result.value());
  return SlottedPage::DeleteRecord(page, rid.slot_id);
}

Result<std::vector<RowEntry>> HeapTable::Scan() {
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
