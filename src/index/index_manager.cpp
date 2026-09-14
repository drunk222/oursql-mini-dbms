#include "oursql/index/index_manager.h"

#include "oursql/storage/heap_table.h"
#include "oursql/transaction/lock_manager.h"

#include <cstdint>
#include <string>

namespace oursql {

namespace {

Status MarkTransactionFailed(Transaction *transaction, Status status) {
  if (transaction != nullptr) transaction->MarkFailed();
  return status;
}

std::uint32_t StableResourceId(std::string_view name) noexcept {
  std::uint32_t hash = 2166136261U;
  for (const unsigned char byte : name) {
    hash ^= byte;
    hash *= 16777619U;
  }
  return hash == 0 ? 1U : hash;
}

index_id_t StableIndexId(std::string_view name) noexcept {
  auto hash = StableResourceId(name);
  hash ^= 0x9e3779b9U;
  return hash == 0 ? 1U : hash;
}

}  // namespace

Result<std::unique_ptr<BPlusTree>> IndexManager::OpenTree(
    const IndexMetadata &metadata, Transaction *transaction) const {
  if (buffer_pool_ == nullptr) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InvalidArgument("IndexManager缺少BufferPoolManager"));
  }
  return Result<std::unique_ptr<BPlusTree>>(
      std::make_unique<BPlusTree>(buffer_pool_, metadata.root_page_id,
                                  BPlusTreeLeafPage::kPhysicalMaxSize,
                                  BPlusTreeInternalPage::kPhysicalMaxSize,
                                  transaction));
}

Result<std::size_t> IndexManager::IndexedColumn(const IndexMetadata &metadata) const {
  if (catalog_ == nullptr) {
    return Result<std::size_t>(Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto table = catalog_->GetTableMetadataCopy(metadata.table_name);
  if (!table.ok()) return Result<std::size_t>(table.status());
  auto column = table.value().schema.FindColumnIndex(metadata.column_name);
  if (!column.ok()) return Result<std::size_t>(column.status());
  if (table.value().schema.At(column.value()).type != DataType::Int) {
    return Result<std::size_t>(Status::TypeMismatch("当前B+树索引仅支持INT列"));
  }
  return Result<std::size_t>(column.value());
}

Status IndexManager::LockUniqueKey(const IndexMetadata &metadata,
                                   index_key_t key,
                                   Transaction *transaction) const {
  if (!metadata.is_unique || transaction == nullptr) return Status::Ok();
  if (lock_manager_ == nullptr) {
    // SQL 编排层可能已经取得相同锁；没有显式 LockManager 依赖时保持兼容。
    // 独立并发调用者应传入 LockManager，使检查与插入之间没有竞争窗口。
    return Status::Ok();
  }
  return lock_manager_->LockIndexKey(
      transaction, StableIndexId(metadata.name), "i:" + std::to_string(key),
      LockMode::X, StableResourceId(metadata.table_name));
}

Status IndexManager::PersistRootIfChanged(const IndexMetadata &metadata,
                                          const BPlusTree &tree,
                                          page_id_t old_root,
                                          Transaction *transaction) {
  if (tree.GetRootPageId() == old_root) return Status::Ok();
  return catalog_->UpdateIndexRootPageId(metadata.name, tree.GetRootPageId(), transaction);
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
  return InsertEntry(index_name, key, rid, nullptr);
}

Status IndexManager::InsertEntry(std::string_view index_name, index_key_t key, RID rid,
                                 Transaction *transaction) {
  if (catalog_ == nullptr) {
    return MarkTransactionFailed(transaction,
                                 Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto found = catalog_->FindIndexCopy(index_name);
  if (!found.ok()) return MarkTransactionFailed(transaction, found.status());
  auto metadata = found.value();
  auto key_lock = LockUniqueKey(metadata, key, transaction);
  if (!key_lock.ok()) return MarkTransactionFailed(transaction, key_lock);
  // 等待唯一 Key 锁期间根可能发生过分裂；锁授予后必须重新读取根元数据。
  if (metadata.is_unique && transaction != nullptr && lock_manager_ != nullptr) {
    auto refreshed = catalog_->FindIndexCopy(index_name);
    if (!refreshed.ok()) return MarkTransactionFailed(transaction, refreshed.status());
    metadata = refreshed.value();
  }
  auto tree_result = OpenTree(metadata, transaction);
  if (!tree_result.ok()) return MarkTransactionFailed(transaction, tree_result.status());
  auto &tree = *tree_result.value();
  if (metadata.is_unique) {
    // IndexKey X 串行化同一逻辑键；这里不再申请索引页 S 锁，避免事务已
    // 持有该叶页 X 锁时发生 X->S 的伪升级。真正写入仍必须取得 Page X。
    auto existing = tree.GetValue(key);
    if (!existing.ok()) return MarkTransactionFailed(transaction, existing.status());
    if (!existing.value().empty()) {
      return MarkTransactionFailed(transaction, Status::AlreadyExists("唯一索引Key已存在"));
    }
  }
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Insert(key, rid);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  status = PersistRootIfChanged(metadata, tree, old_root, transaction);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  return status;
}

Status IndexManager::DeleteEntry(std::string_view index_name, index_key_t key, RID rid) {
  return DeleteEntry(index_name, key, rid, nullptr);
}

Status IndexManager::DeleteEntry(std::string_view index_name, index_key_t key, RID rid,
                                 Transaction *transaction) {
  if (catalog_ == nullptr) {
    return MarkTransactionFailed(transaction,
                                 Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto found = catalog_->FindIndexCopy(index_name);
  if (!found.ok()) return MarkTransactionFailed(transaction, found.status());
  const auto metadata = found.value();
  auto tree_result = OpenTree(metadata, transaction);
  if (!tree_result.ok()) return MarkTransactionFailed(transaction, tree_result.status());
  auto &tree = *tree_result.value();
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Remove(key, rid);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  status = PersistRootIfChanged(metadata, tree, old_root, transaction);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  return status;
}

Status IndexManager::UpdateEntry(std::string_view index_name, index_key_t old_key,
                                 RID old_rid, index_key_t new_key, RID new_rid) {
  return UpdateEntry(index_name, old_key, old_rid, new_key, new_rid, nullptr);
}

Status IndexManager::UpdateEntry(std::string_view index_name, index_key_t old_key,
                                 RID old_rid, index_key_t new_key, RID new_rid,
                                 Transaction *transaction) {
  if (old_key == new_key && old_rid == new_rid) return Status::Ok();
  if (catalog_ == nullptr) {
    return MarkTransactionFailed(transaction,
                                 Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto found = catalog_->FindIndexCopy(index_name);
  if (!found.ok()) return MarkTransactionFailed(transaction, found.status());
  auto metadata = found.value();
  auto key_lock = LockUniqueKey(metadata, new_key, transaction);
  if (!key_lock.ok()) return MarkTransactionFailed(transaction, key_lock);
  if (metadata.is_unique && transaction != nullptr && lock_manager_ != nullptr) {
    auto refreshed = catalog_->FindIndexCopy(index_name);
    if (!refreshed.ok()) return MarkTransactionFailed(transaction, refreshed.status());
    metadata = refreshed.value();
  }
  auto tree_result = OpenTree(metadata, transaction);
  if (!tree_result.ok()) return MarkTransactionFailed(transaction, tree_result.status());
  auto &tree = *tree_result.value();
  if (metadata.is_unique) {
    auto existing = tree.GetValue(new_key);
    if (!existing.ok()) return MarkTransactionFailed(transaction, existing.status());
    for (const auto &candidate : existing.value()) {
      if (candidate != old_rid) {
        return MarkTransactionFailed(transaction,
                                     Status::AlreadyExists("唯一索引Key已存在"));
      }
    }
  }
  const auto old_root = tree.GetRootPageId();
  auto status = tree.Remove(old_key, old_rid);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  status = tree.Insert(new_key, new_rid);
  if (!status.ok()) {
    if (transaction == nullptr) (void)tree.Insert(old_key, old_rid);
    return MarkTransactionFailed(transaction, status);
  }
  status = PersistRootIfChanged(metadata, tree, old_root, transaction);
  if (!status.ok()) return MarkTransactionFailed(transaction, status);
  return status;
}

Status IndexManager::OnInsert(std::string_view table_name, const Row &row, RID rid) {
  return OnInsert(table_name, row, rid, nullptr);
}

Status IndexManager::OnInsert(std::string_view table_name, const Row &row, RID rid,
                              Transaction *transaction) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return MarkTransactionFailed(
        transaction, Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager"));
  }
  struct PreparedEntry {
    IndexMetadata metadata;
    index_key_t key;
  };
  std::vector<PreparedEntry> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return MarkTransactionFailed(transaction, column.status());
    if (column.value() >= row.size()) {
      return MarkTransactionFailed(transaction, Status::InvalidArgument("索引维护行列数不足"));
    }
    const auto key = row[column.value()].AsInt();
    if (metadata.is_unique) {
      auto existing = Lookup(metadata.name, key);
      if (!existing.ok()) return MarkTransactionFailed(transaction, existing.status());
      if (!existing.value().empty()) {
        return MarkTransactionFailed(transaction, Status::AlreadyExists("唯一索引Key已存在"));
      }
    }
    prepared.push_back({metadata, key});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = InsertEntry(prepared[applied].metadata.name, prepared[applied].key, rid,
                              transaction);
    if (!status.ok()) {
      if (transaction == nullptr) {
        while (applied > 0) {
          --applied;
          (void)DeleteEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
        }
      } else {
        transaction->MarkFailed();
      }
      return status;
    }
  }
  return Status::Ok();
}

Status IndexManager::OnDelete(std::string_view table_name, const Row &row, RID rid) {
  return OnDelete(table_name, row, rid, nullptr);
}

Status IndexManager::OnDelete(std::string_view table_name, const Row &row, RID rid,
                              Transaction *transaction) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return MarkTransactionFailed(
        transaction, Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager"));
  }
  struct PreparedEntry {
    IndexMetadata metadata;
    index_key_t key;
  };
  std::vector<PreparedEntry> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return MarkTransactionFailed(transaction, column.status());
    if (column.value() >= row.size()) {
      return MarkTransactionFailed(transaction, Status::InvalidArgument("索引维护行列数不足"));
    }
    prepared.push_back({metadata, row[column.value()].AsInt()});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = DeleteEntry(prepared[applied].metadata.name, prepared[applied].key, rid,
                              transaction);
    if (!status.ok()) {
      if (transaction == nullptr) {
        while (applied > 0) {
          --applied;
          (void)InsertEntry(prepared[applied].metadata.name, prepared[applied].key, rid);
        }
      } else {
        transaction->MarkFailed();
      }
      return status;
    }
  }
  return Status::Ok();
}

Status IndexManager::OnUpdate(std::string_view table_name, const Row &old_row,
                              RID old_rid, const Row &new_row, RID new_rid) {
  return OnUpdate(table_name, old_row, old_rid, new_row, new_rid, nullptr);
}

Status IndexManager::OnUpdate(std::string_view table_name, const Row &old_row,
                              RID old_rid, const Row &new_row, RID new_rid,
                              Transaction *transaction) {
  if (catalog_ == nullptr || buffer_pool_ == nullptr) {
    return MarkTransactionFailed(
        transaction, Status::InvalidArgument("IndexManager需要Catalog和BufferPoolManager"));
  }
  struct PreparedUpdate {
    IndexMetadata metadata;
    index_key_t old_key;
    index_key_t new_key;
  };
  std::vector<PreparedUpdate> prepared;
  for (const auto &metadata : catalog_->ListTableIndexes(table_name)) {
    auto column = IndexedColumn(metadata);
    if (!column.ok()) return MarkTransactionFailed(transaction, column.status());
    if (column.value() >= old_row.size() || column.value() >= new_row.size()) {
      return MarkTransactionFailed(transaction, Status::InvalidArgument("索引维护行列数不足"));
    }
    const auto old_key = old_row[column.value()].AsInt();
    const auto new_key = new_row[column.value()].AsInt();
    if (metadata.is_unique) {
      auto existing = Lookup(metadata.name, new_key);
      if (!existing.ok()) return MarkTransactionFailed(transaction, existing.status());
      for (const auto &candidate : existing.value()) {
        if (candidate != old_rid) {
          return MarkTransactionFailed(transaction,
                                       Status::AlreadyExists("唯一索引Key已存在"));
        }
      }
    }
    prepared.push_back({metadata, old_key, new_key});
  }
  std::size_t applied = 0;
  for (; applied < prepared.size(); ++applied) {
    auto status = UpdateEntry(prepared[applied].metadata.name, prepared[applied].old_key,
                              old_rid, prepared[applied].new_key, new_rid, transaction);
    if (!status.ok()) {
      if (transaction == nullptr) {
        while (applied > 0) {
          --applied;
          (void)UpdateEntry(prepared[applied].metadata.name, prepared[applied].new_key,
                            new_rid, prepared[applied].old_key, old_rid);
        }
      } else {
        transaction->MarkFailed();
      }
      return status;
    }
  }
  return Status::Ok();
}

Result<std::vector<RID>> IndexManager::Lookup(std::string_view index_name,
                                              index_key_t key) const {
  return Lookup(index_name, key, nullptr);
}

Result<std::vector<RID>> IndexManager::Lookup(std::string_view index_name,
                                              index_key_t key,
                                              Transaction *transaction) const {
  if (catalog_ == nullptr) return Result<std::vector<RID>>(Status::InvalidArgument("IndexManager缺少Catalog"));
  auto found = catalog_->FindIndexCopy(index_name);
  if (!found.ok()) return Result<std::vector<RID>>(found.status());
  auto tree = OpenTree(found.value(), transaction);
  if (!tree.ok()) return Result<std::vector<RID>>(tree.status());
  return tree.value()->GetValue(key, transaction);
}

Result<std::vector<std::pair<index_key_t, RID>>> IndexManager::RangeScan(
    std::string_view index_name, index_key_t lower, index_key_t upper) const {
  return RangeScan(index_name, lower, upper, nullptr);
}

Result<std::vector<std::pair<index_key_t, RID>>> IndexManager::RangeScan(
    std::string_view index_name, index_key_t lower, index_key_t upper,
    Transaction *transaction) const {
  if (catalog_ == nullptr) {
    return Result<std::vector<std::pair<index_key_t, RID>>>(
        Status::InvalidArgument("IndexManager缺少Catalog"));
  }
  auto found = catalog_->FindIndexCopy(index_name);
  if (!found.ok()) return Result<std::vector<std::pair<index_key_t, RID>>>(found.status());
  auto tree = OpenTree(found.value(), transaction);
  if (!tree.ok()) return Result<std::vector<std::pair<index_key_t, RID>>>(tree.status());
  return tree.value()->RangeScan(lower, upper, transaction);
}

}  // namespace oursql
