#include "oursql/storage/heap_table.h"

#include <unordered_set>
#include <utility>

namespace oursql {

HeapTable::HeapTable(BufferPoolManager *buffer_pool, TableMetadata metadata) noexcept
    : buffer_pool_(buffer_pool), metadata_(std::move(metadata)) {}

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
  if (buffer_pool_ == nullptr) return Result<std::vector<RowEntry>>(Status::InvalidArgument("HeapTable 缺少 BufferPoolManager"));
  if (metadata_.first_data_page_id == INVALID_PAGE_ID || metadata_.first_data_page_id == 0) {
    return Result<std::vector<RowEntry>>(Status::InvalidArgument("HeapTable 缺少首数据页"));
  }
  std::vector<RowEntry> rows;
  page_id_t current = metadata_.first_data_page_id;
  std::unordered_set<page_id_t> visited;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || !visited.insert(current).second) {
      return Result<std::vector<RowEntry>>(Status::InvalidArgument("数据页链表损坏或成环"));
    }
    auto page_result = buffer_pool_->FetchPage(current);
    if (!page_result.ok()) return Result<std::vector<RowEntry>>(page_result.status());
    auto page = std::move(page_result.value());
    auto valid = SlottedPage::Validate(page);
    if (!valid.ok()) return Result<std::vector<RowEntry>>(valid);
    auto count = SlottedPage::SlotCount(page);
    if (!count.ok()) return Result<std::vector<RowEntry>>(count.status());
    for (std::uint32_t slot = 0; slot < count.value(); ++slot) {
      auto record = SlottedPage::GetRecord(page, slot);
      if (!record.ok()) {
        if (record.status().code() == ErrorCode::NotFound) continue;
        return Result<std::vector<RowEntry>>(record.status());
      }
      auto row = RowCodec::Decode(metadata_.schema, record.value());
      if (!row.ok()) return Result<std::vector<RowEntry>>(row.status());
      rows.emplace_back(RID{current, static_cast<slot_id_t>(slot)}, std::move(row.value()));
    }
    auto next = SlottedPage::NextPageId(page);
    if (!next.ok()) return Result<std::vector<RowEntry>>(next.status());
    current = next.value();
  }
  return Result<std::vector<RowEntry>>(std::move(rows));
}

}  // namespace oursql
