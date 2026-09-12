#include "oursql/catalog/catalog.h"

#include "oursql/storage/storage.h"

#include <limits>
#include <unordered_set>
#include <utility>

namespace oursql {

namespace {

// Catalog 是数据库的“表目录”：tables_ 是运行时内存索引；每个 TableMetadata
// 同时会编码为 Catalog 页面链中的一条记录。用户、权限也可沿用此持久化模式。

constexpr std::uint32_t kCatalogRecordMagic = 0x314C4254U;
constexpr std::uint32_t kIndexRecordMagic = 0x31584449U;  // "IDX1"

// Catalog 记录格式（小端序）：magic、首数据页、表名长度、列数、表名，随后是
// 重复的 [列名长度、类型标签、VARCHAR 上限、列名]；0 表示 VARCHAR 未指定上限。

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
  // 在写盘和恢复后都调用同一校验，保证内存目录只包含合法且列名唯一的表结构。
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
  // 明确逐字段编码，不直接序列化 C++ 对象（其中含 string/vector 等进程内指针）。
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
  // 磁盘内容不可信：每次读取变长字段前都验证剩余字节范围。
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

Status ValidateIndexShape(const IndexMetadata &index) {
  if (index.name.empty()) return Status::InvalidArgument("索引名不能为空");
  if (index.table_name.empty()) return Status::InvalidArgument("索引所属表不能为空");
  if (index.column_name.empty()) return Status::InvalidArgument("索引列不能为空");
  if (index.root_page_id == 0) return Status::InvalidArgument("索引根页面编号不能是superblock");
  return Status::Ok();
}

Result<std::vector<std::byte>> EncodeIndex(const IndexMetadata &index) {
  auto valid = ValidateIndexShape(index);
  if (!valid.ok()) return Result<std::vector<std::byte>>(valid);
  if (index.name.size() > std::numeric_limits<std::uint32_t>::max() ||
      index.table_name.size() > std::numeric_limits<std::uint32_t>::max() ||
      index.column_name.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument("Catalog 索引名称长度超过上限"));
  }
  // magic、root page id、unique、三个字符串长度，随后依次存放索引/表/列名称。
  std::vector<std::byte> bytes;
  AppendU32(&bytes, kIndexRecordMagic);
  AppendU32(&bytes, index.root_page_id);
  AppendU32(&bytes, index.is_unique ? 1U : 0U);
  AppendU32(&bytes, static_cast<std::uint32_t>(index.name.size()));
  AppendU32(&bytes, static_cast<std::uint32_t>(index.table_name.size()));
  AppendU32(&bytes, static_cast<std::uint32_t>(index.column_name.size()));
  for (const auto character : index.name) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  for (const auto character : index.table_name) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  for (const auto character : index.column_name) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
  if (bytes.size() > SlottedPage::kMaxRecordSize) {
    return Result<std::vector<std::byte>>(Status::RecordTooLarge(
        "Catalog 索引元数据超过页面上限: " + std::to_string(SlottedPage::kMaxRecordSize)));
  }
  return Result<std::vector<std::byte>>(std::move(bytes));
}

Result<IndexMetadata> DecodeIndex(const std::vector<std::byte> &bytes) {
  if (!CanRead(bytes, 0, 24) || ReadU32(bytes, 0) != kIndexRecordMagic) {
    return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引记录头损坏"));
  }
  IndexMetadata index;
  index.root_page_id = ReadU32(bytes, 4);
  const auto unique = ReadU32(bytes, 8);
  if (unique > 1) return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引唯一标记非法"));
  index.is_unique = unique == 1;
  const auto index_name_length = ReadU32(bytes, 12);
  const auto table_name_length = ReadU32(bytes, 16);
  const auto column_name_length = ReadU32(bytes, 20);
  std::size_t offset = 24;
  if (!CanRead(bytes, offset, index_name_length)) {
    return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引名越界"));
  }
  index.name.assign(reinterpret_cast<const char *>(bytes.data() + offset), index_name_length);
  offset += index_name_length;
  if (!CanRead(bytes, offset, table_name_length)) {
    return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引表名越界"));
  }
  index.table_name.assign(reinterpret_cast<const char *>(bytes.data() + offset), table_name_length);
  offset += table_name_length;
  if (!CanRead(bytes, offset, column_name_length)) {
    return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引列名越界"));
  }
  index.column_name.assign(reinterpret_cast<const char *>(bytes.data() + offset), column_name_length);
  offset += column_name_length;
  if (offset != bytes.size()) {
    return Result<IndexMetadata>(Status::InvalidArgument("Catalog 索引记录有多余数据"));
  }
  auto valid = ValidateIndexShape(index);
  if (!valid.ok()) return Result<IndexMetadata>(valid);
  return Result<IndexMetadata>(std::move(index));
}

}  // namespace

Status Catalog::Open() {
  if (opened_) return Status::Ok();
  if (buffer_pool_ == nullptr || disk_manager_ == nullptr) {
    return Status::InvalidArgument("持久化 Catalog 需要 BufferPoolManager 和 DiskManager");
  }
  if (!buffer_pool_->GetInitStatus().ok()) return buffer_pool_->GetInitStatus();

  // superblock 保存 Catalog 页链的入口；新库没有入口时创建首个 Catalog 页。
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
    // 先持久化有效的 Catalog 页，再让 superblock 指向它。否则调试器在两次写盘之间
    // 停止进程时，下一次启动会读到一个仍全为零的 Catalog 页。
    auto flush_status = buffer_pool_->FlushPage(new_head);
    if (!flush_status.ok()) {
      return flush_status;
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
  std::vector<IndexMetadata> recovered_indexes;
  // 重启恢复路径：遍历 Catalog 页链，按 magic 区分表记录与索引记录。
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
      if (!CanRead(record.value(), 0, 4)) {
        return Status::InvalidArgument("Catalog 元数据记录头损坏");
      }
      const auto magic = ReadU32(record.value(), 0);
      if (magic == kCatalogRecordMagic) {
        auto table = DecodeTable(record.value());
        if (!table.ok()) return table.status();
        if (tables_.find(table.value().name) != tables_.end()) {
          return Status::AlreadyExists("Catalog 出现重复表: " + table.value().name);
        }
        tables_.emplace(table.value().name, std::move(table.value()));
      } else if (magic == kIndexRecordMagic) {
        auto index = DecodeIndex(record.value());
        if (!index.ok()) return index.status();
        recovered_indexes.push_back(std::move(index.value()));
      } else {
        return Status::InvalidArgument("Catalog 元数据记录类型未知");
      }
    }
    auto next = SlottedPage::NextPageId(page);
    if (!next.ok()) return next.status();
    current = next.value();
  }
  // 索引可能位于另一 Catalog 页，因此在所有表恢复完成后统一校验归属关系。
  for (auto &index : recovered_indexes) {
    if (indexes_.find(index.name) != indexes_.end()) {
      return Status::AlreadyExists("Catalog 出现重复索引: " + index.name);
    }
    const auto table = tables_.find(index.table_name);
    if (table == tables_.end()) {
      return Status::InvalidArgument("Catalog 索引引用不存在的表: " + index.table_name);
    }
    if (!table->second.schema.FindColumn(index.column_name).ok()) {
      return Status::InvalidArgument("Catalog 索引引用不存在的列: " + index.column_name);
    }
    indexes_.emplace(index.name, std::move(index));
  }
  opened_ = true;
  return Status::Ok();
}

Status Catalog::AppendCatalogRecord(const std::vector<std::byte> &record) {
  page_id_t current = catalog_head_;
  std::unordered_set<page_id_t> visited;
  while (true) {
    if (current == 0 || current == INVALID_PAGE_ID || !visited.insert(current).second) {
      return Status::InvalidArgument("Catalog 页面链表损坏或成环");
    }
    auto page_result = buffer_pool_->FetchPageWrite(current);
    if (!page_result.ok()) return page_result.status();
    page_id_t next = INVALID_PAGE_ID;
    {
      auto page = std::move(page_result.value());
      auto slot = SlottedPage::InsertRecord(page, record);
      if (slot.ok()) return Status::Ok();
      if (slot.status().code() != ErrorCode::OutOfSpace) return slot.status();
      auto next_result = SlottedPage::NextPageId(page);
      if (!next_result.ok()) return next_result.status();
      next = next_result.value();
    }
    if (next != INVALID_PAGE_ID) {
      current = next;
      continue;
    }

    auto new_catalog_result = buffer_pool_->NewPageGuarded();
    if (!new_catalog_result.ok()) return new_catalog_result.status();
    page_id_t new_catalog_id = INVALID_PAGE_ID;
    {
      auto new_catalog_page = std::move(new_catalog_result.value());
      new_catalog_id = new_catalog_page.PageId();
      auto init_status = SlottedPage::Initialize(new_catalog_page);
      if (!init_status.ok()) return init_status;
    }
    auto link_result = buffer_pool_->FetchPageWrite(current);
    if (!link_result.ok()) {
      (void)buffer_pool_->DeletePage(new_catalog_id);
      return link_result.status();
    }
    {
      auto link_page = std::move(link_result.value());
      auto link_status = SlottedPage::SetNextPageId(link_page, new_catalog_id);
      if (!link_status.ok()) {
        (void)buffer_pool_->DeletePage(new_catalog_id);
        return link_status;
      }
    }
    current = new_catalog_id;
  }
}

Status Catalog::CreateTable(TableInfo table) {
  auto valid = ValidateTable(table);
  if (!valid.ok()) return valid;
  if (tables_.find(table.name) != tables_.end()) return Status::AlreadyExists("表已存在: " + table.name);

  // 先按不影响记录长度的占位 page_id 编码，超大元数据不得触发任何页面申请。
  auto encoded_size_check = EncodeTable(table);
  if (!encoded_size_check.ok()) return encoded_size_check.status();

  // 无存储依赖时退化为纯内存 Catalog，便于上层组件和单元测试独立使用。
  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent) {
    if (buffer_pool_ == nullptr || disk_manager_ == nullptr) {
      return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
    }
    if (!opened_) {
      auto open_status = Open();
      if (!open_status.ok()) return open_status;
    }
    // 每张新表先分配自己的首数据页，再把该页号与 schema 一起写进 Catalog。
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

    auto persist_status = AppendCatalogRecord(encoded.value());
    if (!persist_status.ok()) {
      (void)buffer_pool_->DeletePage(data_page_id);
      return persist_status;
    }
    tables_.emplace(table.name, table);
    return Status::Ok();
  }

  tables_.emplace(table.name, std::move(table));
  return Status::Ok();
}

Status Catalog::DropTable(std::string_view table_name) {
  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent && (buffer_pool_ == nullptr || disk_manager_ == nullptr)) {
    return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
  }
  if (persistent && !opened_) {
    auto status = Open();
    if (!status.ok()) return status;
  }
  auto table = tables_.find(std::string(table_name));
  if (table == tables_.end()) {
    return Status::NotFound("Catalog 未找到表: " + std::string(table_name));
  }
  if (!ListTableIndexes(table_name).empty()) {
    return Status::InvalidArgument("删除表元数据前必须先删除该表的全部索引");
  }
  if (persistent) {
    page_id_t record_page_id = INVALID_PAGE_ID;
    slot_id_t record_slot_id = INVALID_SLOT_ID;
    std::unordered_set<page_id_t> visited;
    page_id_t current = catalog_head_;
    while (current != INVALID_PAGE_ID && record_page_id == INVALID_PAGE_ID) {
      if (current == 0 || !visited.insert(current).second) {
        return Status::InvalidArgument("Catalog 页面链表损坏或成环");
      }
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      auto page = std::move(page_result.value());
      auto count = SlottedPage::SlotCount(page);
      if (!count.ok()) return count.status();
      for (std::uint32_t slot = 0; slot < count.value(); ++slot) {
        auto record = SlottedPage::GetRecord(page, slot);
        if (!record.ok()) {
          if (record.status().code() == ErrorCode::NotFound) continue;
          return record.status();
        }
        if (!CanRead(record.value(), 0, 4) ||
            ReadU32(record.value(), 0) != kCatalogRecordMagic) {
          continue;
        }
        auto decoded = DecodeTable(record.value());
        if (!decoded.ok()) return decoded.status();
        if (decoded.value().name == table_name) {
          record_page_id = current;
          record_slot_id = slot;
          break;
        }
      }
      auto next = SlottedPage::NextPageId(page);
      if (!next.ok()) return next.status();
      current = next.value();
    }
    if (record_page_id == INVALID_PAGE_ID) {
      return Status::InternalError("Catalog 内存表缺少对应的磁盘记录");
    }
    auto page_result = buffer_pool_->FetchPageWrite(record_page_id);
    if (!page_result.ok()) return page_result.status();
    auto page = std::move(page_result.value());
    auto status = SlottedPage::DeleteRecord(page, record_slot_id);
    if (!status.ok()) return status;
  }
  tables_.erase(table);
  return Status::Ok();
}

Status Catalog::CreateIndex(IndexInfo index) {
  auto valid = ValidateIndexShape(index);
  if (!valid.ok()) return valid;
  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent) {
    if (buffer_pool_ == nullptr || disk_manager_ == nullptr) {
      return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
    }
    if (!opened_) {
      auto open_status = Open();
      if (!open_status.ok()) return open_status;
    }
  }
  if (indexes_.find(index.name) != indexes_.end()) {
    return Status::AlreadyExists("索引已存在: " + index.name);
  }
  const auto table = tables_.find(index.table_name);
  if (table == tables_.end()) {
    return Status::NotFound("索引所属表不存在: " + index.table_name);
  }
  auto column = table->second.schema.FindColumn(index.column_name);
  if (!column.ok()) {
    return Status::NotFound("索引列不存在: " + index.table_name + "." + index.column_name);
  }
  auto encoded = EncodeIndex(index);
  if (!encoded.ok()) return encoded.status();
  if (persistent) {
    auto persist_status = AppendCatalogRecord(encoded.value());
    if (!persist_status.ok()) return persist_status;
  }
  indexes_.emplace(index.name, std::move(index));
  return Status::Ok();
}

Status Catalog::UpdateIndexRootPageId(std::string_view index_name,
                                      page_id_t root_page_id) {
  if (root_page_id == 0) return Status::InvalidArgument("索引根页面编号不能是superblock");
  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent && (buffer_pool_ == nullptr || disk_manager_ == nullptr)) {
    return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
  }
  if (persistent && !opened_) {
    auto open_status = Open();
    if (!open_status.ok()) return open_status;
  }
  auto index = indexes_.find(std::string(index_name));
  if (index == indexes_.end()) {
    return Status::NotFound("Catalog 未找到索引: " + std::string(index_name));
  }
  IndexMetadata updated = index->second;
  updated.root_page_id = root_page_id;
  auto encoded = EncodeIndex(updated);
  if (!encoded.ok()) return encoded.status();

  if (persistent) {
    page_id_t record_page_id = INVALID_PAGE_ID;
    slot_id_t record_slot_id = INVALID_SLOT_ID;
    std::unordered_set<page_id_t> visited;
    page_id_t current = catalog_head_;
    while (current != INVALID_PAGE_ID && record_page_id == INVALID_PAGE_ID) {
      if (current == 0 || !visited.insert(current).second) {
        return Status::InvalidArgument("Catalog 页面链表损坏或成环");
      }
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      auto page = std::move(page_result.value());
      auto count = SlottedPage::SlotCount(page);
      if (!count.ok()) return count.status();
      for (std::uint32_t slot = 0; slot < count.value(); ++slot) {
        auto record = SlottedPage::GetRecord(page, slot);
        if (!record.ok()) {
          if (record.status().code() == ErrorCode::NotFound) continue;
          return record.status();
        }
        if (!CanRead(record.value(), 0, 4) ||
            ReadU32(record.value(), 0) != kIndexRecordMagic) {
          continue;
        }
        auto decoded = DecodeIndex(record.value());
        if (!decoded.ok()) return decoded.status();
        if (decoded.value().name == index_name) {
          record_page_id = current;
          record_slot_id = slot;
          break;
        }
      }
      auto next = SlottedPage::NextPageId(page);
      if (!next.ok()) return next.status();
      current = next.value();
    }
    if (record_page_id == INVALID_PAGE_ID) {
      return Status::InternalError("Catalog 内存索引缺少对应的磁盘记录");
    }
    auto page_result = buffer_pool_->FetchPageWrite(record_page_id);
    if (!page_result.ok()) return page_result.status();
    auto page = std::move(page_result.value());
    auto delete_status = SlottedPage::DeleteRecord(page, record_slot_id);
    if (!delete_status.ok()) return delete_status;
    auto inserted = SlottedPage::InsertRecord(page, encoded.value());
    if (!inserted.ok()) {
      return Status::InternalError("更新Catalog索引根页面记录失败: " +
                                   inserted.status().ToString());
    }
  }
  index->second.root_page_id = root_page_id;
  return Status::Ok();
}

Status Catalog::DropIndex(std::string_view index_name) {
  const bool persistent = buffer_pool_ != nullptr || disk_manager_ != nullptr;
  if (persistent && (buffer_pool_ == nullptr || disk_manager_ == nullptr)) {
    return Status::InvalidArgument("持久化 Catalog 需要完整的存储依赖");
  }
  if (persistent && !opened_) {
    auto status = Open();
    if (!status.ok()) return status;
  }
  auto index = indexes_.find(std::string(index_name));
  if (index == indexes_.end()) {
    return Status::NotFound("Catalog 未找到索引: " + std::string(index_name));
  }
  if (persistent) {
    page_id_t record_page_id = INVALID_PAGE_ID;
    slot_id_t record_slot_id = INVALID_SLOT_ID;
    std::unordered_set<page_id_t> visited;
    page_id_t current = catalog_head_;
    while (current != INVALID_PAGE_ID && record_page_id == INVALID_PAGE_ID) {
      if (current == 0 || !visited.insert(current).second) {
        return Status::InvalidArgument("Catalog 页面链表损坏或成环");
      }
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      auto page = std::move(page_result.value());
      auto count = SlottedPage::SlotCount(page);
      if (!count.ok()) return count.status();
      for (std::uint32_t slot = 0; slot < count.value(); ++slot) {
        auto record = SlottedPage::GetRecord(page, slot);
        if (!record.ok()) {
          if (record.status().code() == ErrorCode::NotFound) continue;
          return record.status();
        }
        if (!CanRead(record.value(), 0, 4) || ReadU32(record.value(), 0) != kIndexRecordMagic) {
          continue;
        }
        auto decoded = DecodeIndex(record.value());
        if (!decoded.ok()) return decoded.status();
        if (decoded.value().name == index_name) {
          record_page_id = current;
          record_slot_id = slot;
          break;
        }
      }
      auto next = SlottedPage::NextPageId(page);
      if (!next.ok()) return next.status();
      current = next.value();
    }
    if (record_page_id == INVALID_PAGE_ID) {
      return Status::InternalError("Catalog 内存索引缺少对应的磁盘记录");
    }
    auto page_result = buffer_pool_->FetchPageWrite(record_page_id);
    if (!page_result.ok()) return page_result.status();
    auto page = std::move(page_result.value());
    auto status = SlottedPage::DeleteRecord(page, record_slot_id);
    if (!status.ok()) return status;
  }
  indexes_.erase(index);
  return Status::Ok();
}

Result<const TableInfo *> Catalog::FindTable(std::string_view table_name) const {
  // 返回 map 内对象的只读指针；调用方不得跨越 Catalog 生命周期保存它。
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

Result<const IndexMetadata *> Catalog::FindIndex(std::string_view index_name) const {
  auto it = indexes_.find(std::string(index_name));
  if (it == indexes_.end()) {
    return Result<const IndexMetadata *>(
        Status::NotFound("Catalog 未找到索引: " + std::string(index_name)));
  }
  return Result<const IndexMetadata *>(&it->second);
}

bool Catalog::HasIndex(std::string_view index_name) const {
  return indexes_.find(std::string(index_name)) != indexes_.end();
}

Result<const IndexMetadata *> Catalog::GetIndexMetadata(std::string_view index_name) const {
  return FindIndex(index_name);
}

std::vector<IndexMetadata> Catalog::ListIndexes() const {
  std::vector<IndexMetadata> result;
  result.reserve(indexes_.size());
  for (const auto &entry : indexes_) result.push_back(entry.second);
  return result;
}

std::vector<IndexMetadata> Catalog::ListTableIndexes(std::string_view table_name) const {
  std::vector<IndexMetadata> result;
  for (const auto &entry : indexes_) {
    if (entry.second.table_name == table_name) result.push_back(entry.second);
  }
  return result;
}

}  // namespace oursql
