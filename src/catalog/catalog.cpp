#include "oursql/catalog/catalog.h"

#include "oursql/storage/storage.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace oursql {

namespace {

constexpr std::uint32_t kCatalogRecordMagic = 0x314C4254U;

void AppendU32(std::vector<std::byte> *bytes, std::uint32_t value) {
  bytes->push_back(std::byte{static_cast<unsigned char>(value & 0xFFU)});
  bytes->push_back(std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)});
  bytes->push_back(std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)});
  bytes->push_back(std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)});
}

bool CanRead(const std::vector<std::byte> &bytes, std::size_t offset, std::size_t length) {
  return offset <= bytes.size() && length <= bytes.size() - offset;
}

std::uint32_t ReadU32(const std::vector<std::byte> &bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

Status ValidateTable(const TableMetadata &table) {
  if (table.name.empty()) return Status::InvalidArgument("表名不能为空");
  if (table.schema.size() == 0) return Status::InvalidArgument("表结构不能为空");
  std::unordered_set<std::string> names;
  for (const auto &column : table.schema.columns()) {
    if (column.name.empty()) return Status::InvalidArgument("列名不能为空");
    if (!names.insert(column.name).second) return Status::AlreadyExists("重复列名: " + column.name);
    if (column.type == DataType::Int && column.length.has_value()) {
      return Status::InvalidArgument("INT 列不能设置长度: " + column.name);
    }
    if (column.type == DataType::Varchar && column.length.has_value() && *column.length == 0) {
      return Status::InvalidArgument("VARCHAR 长度必须大于 0: " + column.name);
    }
  }
  return Status::Ok();
}

Result<std::vector<std::byte>> EncodeTable(const TableMetadata &table) {
  if (table.name.size() > std::numeric_limits<std::uint32_t>::max() ||
      table.schema.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument("Catalog 表元数据长度超过上限"));
  }
  std::vector<std::byte> bytes;
  AppendU32(&bytes, kCatalogRecordMagic);
  AppendU32(&bytes, table.first_data_page_id);
  AppendU32(&bytes, static_cast<std::uint32_t>(table.name.size()));
  AppendU32(&bytes, static_cast<std::uint32_t>(table.schema.size()));
  for (const auto character : table.name) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  for (const auto &column : table.schema.columns()) {
    if (column.name.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Result<std::vector<std::byte>>(Status::InvalidArgument("Catalog 列名长度超过上限"));
    }
    if (column.length.has_value() && *column.length > std::numeric_limits<std::uint32_t>::max()) {
      return Result<std::vector<std::byte>>(Status::InvalidArgument("Catalog VARCHAR 长度超过上限: " + column.name));
    }
    AppendU32(&bytes, static_cast<std::uint32_t>(column.name.size()));
    bytes.push_back(std::byte{static_cast<unsigned char>(column.type == DataType::Int ? 1U : 2U)});
    AppendU32(&bytes, column.length.has_value() ? static_cast<std::uint32_t>(*column.length) : 0U);
    for (const auto character : column.name) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  }
  if (bytes.size() > SlottedPage::kMaxRecordSize) {
    return Result<std::vector<std::byte>>(Status::RecordTooLarge(
        "Catalog 表元数据超过页面上限: " + std::to_string(SlottedPage::kMaxRecordSize)));
  }
  return Result<std::vector<std::byte>>(std::move(bytes));
}

Result<TableMetadata> DecodeTable(const std::vector<std::byte> &bytes) {
  if (!CanRead(bytes, 0, 16) || ReadU32(bytes, 0) != kCatalogRecordMagic) {
    return Result<TableMetadata>(Status::InvalidArgument("Catalog 表记录头损坏"));
  }
  TableMetadata table;
  table.first_data_page_id = ReadU32(bytes, 4);
  const auto table_name_length = ReadU32(bytes, 8);
  const auto column_count = ReadU32(bytes, 12);
  if (table.first_data_page_id == 0 || table.first_data_page_id == INVALID_PAGE_ID) {
    return Result<TableMetadata>(Status::InvalidArgument("Catalog 首数据页编号非法"));
  }
  std::size_t offset = 16;
  if (!CanRead(bytes, offset, table_name_length)) {
    return Result<TableMetadata>(Status::InvalidArgument("Catalog 表名越界"));
  }
  table.name.assign(reinterpret_cast<const char *>(bytes.data() + offset), table_name_length);
  offset += table_name_length;
  std::vector<Column> columns;
  columns.reserve(column_count);
  for (std::uint32_t i = 0; i < column_count; ++i) {
    if (!CanRead(bytes, offset, 9)) {
      return Result<TableMetadata>(Status::InvalidArgument("Catalog 列定义头越界"));
    }
    const auto name_length = ReadU32(bytes, offset);
    const auto type_tag = static_cast<std::uint8_t>(bytes[offset + 4]);
    const auto varchar_length = ReadU32(bytes, offset + 5);
    offset += 9;
    if (!CanRead(bytes, offset, name_length)) {
      return Result<TableMetadata>(Status::InvalidArgument("Catalog 列名越界"));
    }
    std::string name(reinterpret_cast<const char *>(bytes.data() + offset), name_length);
    offset += name_length;
    if (type_tag == 1U) {
      if (varchar_length != 0) return Result<TableMetadata>(Status::InvalidArgument("Catalog INT 列带有长度"));
      columns.emplace_back(std::move(name), DataType::Int);
    } else if (type_tag == 2U) {
      columns.emplace_back(std::move(name), DataType::Varchar,
                           varchar_length == 0 ? std::nullopt
                                               : std::optional<std::size_t>(varchar_length));
    } else {
      return Result<TableMetadata>(Status::InvalidArgument("Catalog 列类型标记非法"));
    }
  }
  if (offset != bytes.size()) return Result<TableMetadata>(Status::InvalidArgument("Catalog 表记录有多余数据"));
  table.schema = Schema(std::move(columns));
  auto valid = ValidateTable(table);
  if (!valid.ok()) return Result<TableMetadata>(valid);
  return Result<TableMetadata>(std::move(table));
}

}  // namespace

Status Catalog::Open() {
  if (opened_) return Status::Ok();
  if (buffer_pool_ == nullptr || disk_manager_ == nullptr) {
    return Status::InvalidArgument("持久化 Catalog 需要 BufferPoolManager 和 DiskManager");
  }
  if (!buffer_pool_->GetInitStatus().ok()) return buffer_pool_->GetInitStatus();

  catalog_head_ = disk_manager_->GetCatalogHead();
  if (catalog_head_ == INVALID_PAGE_ID) {
    auto page_result = buffer_pool_->NewPageGuarded();
    if (!page_result.ok()) return page_result.status();
    page_id_t new_head = INVALID_PAGE_ID;
    {
      auto page = std::move(page_result.value());
      new_head = page.PageId();
      auto init_status = SlottedPage::Initialize(page);
      if (!init_status.ok()) {
        return init_status;
      }
    }
    auto set_status = disk_manager_->SetCatalogHead(new_head);
    if (!set_status.ok()) {
      (void)buffer_pool_->DeletePage(new_head);
      return set_status;
    }
    catalog_head_ = new_head;
    opened_ = true;
    return Status::Ok();
  }

  std::unordered_set<page_id_t> visited;
  page_id_t current = catalog_head_;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || !visited.insert(current).second) {
      return Status::InvalidArgument("Catalog 页面链表损坏或成环");
    }
    auto page_result = buffer_pool_->FetchPage(current);
    if (!page_result.ok()) return page_result.status();
    auto page = std::move(page_result.value());
    auto valid = SlottedPage::Validate(page);
    if (!valid.ok()) return valid;
    auto count = SlottedPage::SlotCount(page);
    if (!count.ok()) return count.status();
    for (std::uint32_t slot = 0; slot < count.value(); ++slot) {
      auto record = SlottedPage::GetRecord(page, slot);
      if (!record.ok()) {
        if (record.status().code() == ErrorCode::NotFound) continue;
        return record.status();
      }
      auto table = DecodeTable(record.value());
      if (!table.ok()) return table.status();
      if (tables_.find(table.value().name) != tables_.end()) {
        return Status::AlreadyExists("Catalog 出现重复表: " + table.value().name);
      }
      tables_.emplace(table.value().name, std::move(table.value()));
    }
    auto next = SlottedPage::NextPageId(page);
    if (!next.ok()) return next.status();
    current = next.value();
  }
  opened_ = true;
  return Status::Ok();
}

Status Catalog::CreateTable(TableInfo table) {
  auto valid = ValidateTable(table);
  if (!valid.ok()) return valid;
  if (tables_.find(table.name) != tables_.end()) return Status::AlreadyExists("表已存在: " + table.name);

  // 先按不影响记录长度的占位 page_id 编码，超大元数据不得触发任何页面申请。
  auto encoded_size_check = EncodeTable(table);
  if (!encoded_size_check.ok()) return encoded_size_check.status();

  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent) {
    if (buffer_pool_ == nullptr || disk_manager_ == nullptr) {
      return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
    }
    if (!opened_) {
      auto open_status = Open();
      if (!open_status.ok()) return open_status;
    }
    auto data_page_result = buffer_pool_->NewPageGuarded();
    if (!data_page_result.ok()) return data_page_result.status();
    page_id_t data_page_id = INVALID_PAGE_ID;
    {
      auto data_page = std::move(data_page_result.value());
      data_page_id = data_page.PageId();
      auto init_status = SlottedPage::Initialize(data_page);
      if (!init_status.ok()) return init_status;
    }
    table.first_data_page_id = data_page_id;
    auto encoded = EncodeTable(table);
    if (!encoded.ok()) {
      (void)buffer_pool_->DeletePage(data_page_id);
      return encoded.status();
    }

    page_id_t current = catalog_head_;
    std::unordered_set<page_id_t> visited;
    while (true) {
      if (current == 0 || current == INVALID_PAGE_ID || !visited.insert(current).second) {
        (void)buffer_pool_->DeletePage(data_page_id);
        return Status::InvalidArgument("Catalog 页面链表损坏或成环");
      }
      auto page_result = buffer_pool_->FetchPageWrite(current);
      if (!page_result.ok()) {
        (void)buffer_pool_->DeletePage(data_page_id);
        return page_result.status();
      }
      page_id_t next = INVALID_PAGE_ID;
      bool inserted = false;
      {
        auto page = std::move(page_result.value());
        auto slot = SlottedPage::InsertRecord(page, encoded.value());
        if (slot.ok()) {
          inserted = true;
        } else if (slot.status().code() != ErrorCode::OutOfSpace) {
          (void)buffer_pool_->DeletePage(data_page_id);
          return slot.status();
        } else {
          auto next_result = SlottedPage::NextPageId(page);
          if (!next_result.ok()) {
            (void)buffer_pool_->DeletePage(data_page_id);
            return next_result.status();
          }
          next = next_result.value();
        }
      }
      if (inserted) {
        tables_.emplace(table.name, table);
        return Status::Ok();
      }
      if (next != INVALID_PAGE_ID) {
        current = next;
        continue;
      }

      auto new_catalog_result = buffer_pool_->NewPageGuarded();
      if (!new_catalog_result.ok()) {
        (void)buffer_pool_->DeletePage(data_page_id);
        return new_catalog_result.status();
      }
      page_id_t new_catalog_id = INVALID_PAGE_ID;
      {
        auto new_catalog_page = std::move(new_catalog_result.value());
        new_catalog_id = new_catalog_page.PageId();
        auto init_status = SlottedPage::Initialize(new_catalog_page);
        if (!init_status.ok()) {
          (void)buffer_pool_->DeletePage(data_page_id);
          return init_status;
        }
      }
      auto link_result = buffer_pool_->FetchPageWrite(current);
      if (!link_result.ok()) {
        (void)buffer_pool_->DeletePage(new_catalog_id);
        (void)buffer_pool_->DeletePage(data_page_id);
        return link_result.status();
      }
      {
        auto link_page = std::move(link_result.value());
        auto link_status = SlottedPage::SetNextPageId(link_page, new_catalog_id);
        if (!link_status.ok()) {
          (void)buffer_pool_->DeletePage(new_catalog_id);
          (void)buffer_pool_->DeletePage(data_page_id);
          return link_status;
        }
      }
      current = new_catalog_id;
    }
  }

  tables_.emplace(table.name, std::move(table));
  return Status::Ok();
}

Result<const TableInfo *> Catalog::FindTable(std::string_view table_name) const {
  auto it = tables_.find(std::string(table_name));
  if (it == tables_.end()) {
    return Result<const TableInfo *>(Status::NotFound("Catalog 未找到表: " + std::string(table_name)));
  }
  return Result<const TableInfo *>(&it->second);
}

bool Catalog::HasTable(std::string_view table_name) const {
  return tables_.find(std::string(table_name)) != tables_.end();
}

std::vector<TableMetadata> Catalog::ListTables() const {
  std::vector<TableMetadata> result;
  result.reserve(tables_.size());
  for (const auto &entry : tables_) result.push_back(entry.second);
  return result;
}

Result<const Schema *> Catalog::GetSchema(std::string_view table_name) const {
  auto table = FindTable(table_name);
  if (!table.ok()) return Result<const Schema *>(table.status());
  return Result<const Schema *>(&table.value()->schema);
}

Result<const TableMetadata *> Catalog::GetTableMetadata(std::string_view table_name) const {
  auto table = FindTable(table_name);
  if (!table.ok()) return Result<const TableMetadata *>(table.status());
  return Result<const TableMetadata *>(table.value());
}

}  // namespace oursql
