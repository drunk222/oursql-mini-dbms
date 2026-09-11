#include "oursql/index/index_manager.h"

#include "oursql/storage/heap_table.h"

#include <string>

namespace oursql {

Result<std::unique_ptr<BPlusTree>> IndexManager::OpenTree(
    const IndexMetadata &metadata) const {
  if (buffer_pool_ == nullptr) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InvalidArgument("IndexManager缺少BufferPoolManager"));
  }
  return Result<std::unique_ptr<BPlusTree>>(
      std::make_unique<BPlusTree>(buffer_pool_, metadata.root_page_id));
}

Result<std::size_t> IndexManager::IndexedColumn(const IndexMetadata &metadata) const {
  if (catalog_ == nullptr) {
    return Result<std::size_t>(Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto table = catalog_->GetTableMetadata(metadata.table_name);
  if (!table.ok()) return Result<std::size_t>(table.status());
  auto column = table.value()->schema.FindColumnIndex(metadata.column_name);
  if (!column.ok()) return Result<std::size_t>(column.status());
  if (table.value()->schema.At(column.value()).type != DataType::Int) {
    return Result<std::size_t>(Status::TypeMismatch("当前B+树索引仅支持INT列"));
  }
  return Result<std::size_t>(column.value());
}

Status IndexManager::PersistRootIfChanged(const IndexMetadata &metadata,
                                          const BPlusTree &tree,
                                          page_id_t old_root) {
  if (tree.GetRootPageId() == old_root) return Status::Ok();
  return catalog_->UpdateIndexRootPageId(metadata.name, tree.GetRootPageId());
}

Status IndexManager::CreateIndex(std::string_view index_name,
                                 std::string_view table_name,
                                 std::string_view column_name, bool is_unique) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager");
  }
  if (catalog_->HasIndex(index_name)) {
    return Status::AlreadyExists("索引已存在: " + std::string(index_name));
  }
  auto table = catalog_->GetTableMetadata(table_name);
  if (!table.ok()) return table.status();
  auto column = table.value()->schema.FindColumnIndex(column_name);
  if (!column.ok()) return column.status();
  if (table.value()->schema.At(column.value()).type != DataType::Int) {
    return Status::TypeMismatch("当前B+树索引仅支持INT列");
  }

  BPlusTree tree(buffer_pool_);
  HeapTable heap(buffer_pool_, *table.value());
  auto cursor_result = heap.BeginScan();
  if (!cursor_result.ok()) return cursor_result.status();
  auto cursor = std::move(cursor_result.value());
  while (true) {
    auto next = cursor.Next();
    if (!next.ok()) {
      (void)tree.Destroy();
      return next.status();
    }
    if (!next.value().has_value()) break;
    const auto &entry = *next.value();
    const auto key = entry.second[column.value()].AsInt();
    if (is_unique) {
      auto existing = tree.GetValue(key);
      if (!existing.ok()) {
        (void)tree.Destroy();
        return existing.status();
      }
      if (!existing.value().empty()) {
        (void)tree.Destroy();
        return Status::AlreadyExists("唯一索引列存在重复值: " + std::to_string(key));
      }
    }
    auto status = tree.Insert(key, entry.first);
    if (!status.ok()) {
      (void)tree.Destroy();
      return status;
    }
  }
  IndexMetadata metadata{std::string(index_name), std::string(table_name),
                         std::string(column_name), tree.GetRootPageId(), is_unique};
  auto status = catalog_->CreateIndex(metadata);
  if (!status.ok()) {
    (void)tree.Destroy();
    return status;
  }
  return Status::Ok();
}

Status IndexManager::DropIndex(std::string_view index_name) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager");
  }
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return found.status();
  const auto metadata = *found.value();
  auto tree_result = OpenTree(metadata);
  if (!tree_result.ok()) return tree_result.status();
  // 先移除可见元数据；页面释放失败最多产生不可达页，不会留下悬空索引定义。
  auto status = catalog_->DropIndex(index_name);
  if (!status.ok()) return status;
  return tree_result.value()->Destroy();
}

Status IndexManager::InsertEntry(std::string_view index_name, index_key_t key, RID rid) {
  if (catalog_ == nullptr) return Status::InvalidArgument("IndexManager缺少Catalog");
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return found.status();
  const auto metadata = *found.value();
  auto tree_result = OpenTree(metadata);
  if (!tree_result.ok()) return tree_result.status();
  auto &tree = *tree_result.value();
  if (metadata.is_unique) {
    auto existing = tree.GetValue(key);
    if (!existing.ok()) return existing.status();
    if (!existing.value().empty()) return Status::AlreadyExists("唯一索引Key已存在");
  }
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Insert(key, rid);
  if (!status.ok()) return status;
  return PersistRootIfChanged(metadata, tree, old_root);
}

Status IndexManager::DeleteEntry(std::string_view index_name, index_key_t key, RID rid) {
  if (catalog_ == nullptr) return Status::InvalidArgument("IndexManager缺少Catalog");
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return found.status();
  const auto metadata = *found.value();
  auto tree_result = OpenTree(metadata);
  if (!tree_result.ok()) return tree_result.status();
  auto &tree = *tree_result.value();
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Remove(key, rid);
  if (!status.ok()) return status;
  return PersistRootIfChanged(metadata, tree, old_root);
}

Status IndexManager::UpdateEntry(std::string_view index_name, index_key_t old_key,
                                 RID old_rid, index_key_t new_key, RID new_rid) {
  if (old_key == new_key && old_rid == new_rid) return Status::Ok();
  if (catalog_ == nullptr) return Status::InvalidArgument("IndexManager缺少Catalog");
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return found.status();
  const auto metadata = *found.value();
  auto tree_result = OpenTree(metadata);
  if (!tree_result.ok()) return tree_result.status();
  auto &tree = *tree_result.value();
  if (metadata.is_unique) {
    auto existing = tree.GetValue(new_key);
    if (!existing.ok()) return existing.status();
    for (const auto &candidate : existing.value()) {
      if (candidate != old_rid) return Status::AlreadyExists("唯一索引Key已存在");
    }
  }
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Remove(old_key, old_rid);
  if (!status.ok()) return status;
  status = tree.Insert(new_key, new_rid);
  if (!status.ok()) {
    (void)tree.Insert(old_key, old_rid);
    return status;
  }
  return PersistRootIfChanged(metadata, tree, old_root);
}

Status IndexManager::OnInsert(std::string_view table_name, const Row &row, RID rid) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager");
  }
  struct PreparedEntry {
    IndexMetadata metadata;
    index_key_t key;
  };
  std::vector<PreparedEntry> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return column.status();
    if (column.value() >= row.size()) return Status::InvalidArgument("索引维护行列数不足");
    const auto key = row[column.value()].AsInt();
    if (metadata.is_unique) {
      auto existing = Lookup(metadata.name, key);
      if (!existing.ok()) return existing.status();
      if (!existing.value().empty()) return Status::AlreadyExists("唯一索引Key已存在");
    }
    prepared.push_back({metadata, key});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = InsertEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
    if (!status.ok()) {
      while (applied > 0) {
        --applied;
        (void)DeleteEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
      }
      return status;
    }
  }
  return Status::Ok();
}

Status IndexManager::OnDelete(std::string_view table_name, const Row &row, RID rid) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager");
  }
  struct PreparedEntry {
    IndexMetadata metadata;
    index_key_t key;
  };
  std::vector<PreparedEntry> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return column.status();
    if (column.value() >= row.size()) return Status::InvalidArgument("索引维护行列数不足");
    prepared.push_back({metadata, row[column.value()].AsInt()});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = DeleteEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
    if (!status.ok()) {
      while (applied > 0) {
        --applied;
        (void)InsertEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
      }
      return status;
    }
  }
  return Status::Ok();
}

Status IndexManager::OnUpdate(std::string_view table_name, const Row &old_row,
                              RID old_rid, const Row &new_row, RID new_rid) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager");
  }
  struct PreparedUpdate {
    IndexMetadata metadata;
    index_key_t old_key;
    index_key_t new_key;
  };
  std::vector<PreparedUpdate> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return column.status();
    if (column.value() >= old_row.size() || column.value() >= new_row.size()) {
      return Status::InvalidArgument("索引维护行列数不足");
    }
    const auto old_key = old_row[column.value()].AsInt();
    const auto new_key = new_row[column.value()].AsInt();
    if (metadata.is_unique) {
      auto existing = Lookup(metadata.name, new_key);
      if (!existing.ok()) return existing.status();
      for (const auto &candidate : existing.value()) {
        if (candidate != old_rid) return Status::AlreadyExists("唯一索引Key已存在");
      }
    }
    prepared.push_back({metadata, old_key, new_key});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = UpdateEntry(prepared[applied].metadata.name, prepared[applied].old_key,
                              old_rid, prepared[applied].new_key, new_rid);
    if (!status.ok()) {
      while (applied > 0) {
        --applied;
        (void)UpdateEntry(prepared[applied].metadata.name, prepared[applied].new_key,
                          new_rid, prepared[applied].old_key, old_rid);
      }
      return status;
    }
  }
  return Status::Ok();
}

Result<std::vector<RID>> IndexManager::Lookup(std::string_view index_name,
                                              index_key_t key) const {
  if (catalog_ == nullptr) return Result<std::vector<RID>>(Status::InvalidArgument("IndexManager缺少Catalog"));
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return Result<std::vector<RID>>(found.status());
  auto tree = OpenTree(*found.value());
  if (!tree.ok()) return Result<std::vector<RID>>(tree.status());
  return tree.value()->GetValue(key);
}

Result<std::vector<std::pair<index_key_t, RID>>> IndexManager::RangeScan(
    std::string_view index_name, index_key_t lower, index_key_t upper) const {
  if (catalog_ == nullptr) {
    return Result<std::vector<std::pair<index_key_t, RID>>>(
        Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto found = catalog_->FindIndex(index_name);
  if (!found.ok()) return Result<std::vector<std::pair<index_key_t, RID>>>(found.status());
  auto tree = OpenTree(*found.value());
  if (!tree.ok()) return Result<std::vector<std::pair<index_key_t, RID>>>(tree.status());
  return tree.value()->RangeScan(lower, upper);
}

}  // namespace oursql
