#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/index/b_plus_tree.h"
#include "oursql/storage/row_codec.h"
#include "oursql/transaction/transaction.h"

#include <memory>
#include <string_view>
#include <vector>

namespace oursql {

class LockManager;

// IndexManager 连接 Catalog、HeapTable 与 B+树；不参与 SQL 解析、Planner 或存储实现。
class IndexManager {
 public:
  // 直接并发维护唯一索引时传入 LockManager，由本层在唯一性检查前取得
  // IndexKey X；SQL 执行路径也可由上层按统一顺序预先取得同一把锁。
  IndexManager(Catalog *catalog, BufferPoolManager *buffer_pool,
               LockManager *lock_manager = nullptr) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool), lock_manager_(lock_manager) {}

  [[nodiscard]] Status CreateIndex(std::string_view index_name,
                                   std::string_view table_name,
                                   std::string_view column_name, bool is_unique);
  [[nodiscard]] Status DropIndex(std::string_view index_name);

  [[nodiscard]] Status InsertEntry(std::string_view index_name, index_key_t key, RID rid);
  [[nodiscard]] Status InsertEntry(std::string_view index_name, index_key_t key, RID rid,
                                   Transaction *transaction);
  [[nodiscard]] Status DeleteEntry(std::string_view index_name, index_key_t key, RID rid);
  [[nodiscard]] Status DeleteEntry(std::string_view index_name, index_key_t key, RID rid,
                                   Transaction *transaction);
  [[nodiscard]] Status UpdateEntry(std::string_view index_name, index_key_t old_key,
                                   RID old_rid, index_key_t new_key, RID new_rid);
  [[nodiscard]] Status UpdateEntry(std::string_view index_name, index_key_t old_key,
                                   RID old_rid, index_key_t new_key, RID new_rid,
                                   Transaction *transaction);

  // 行级维护接口供 INSERT/DELETE/UPDATE 执行路径在后续接线时调用。
  [[nodiscard]] Status OnInsert(std::string_view table_name, const Row &row, RID rid);
  [[nodiscard]] Status OnInsert(std::string_view table_name, const Row &row, RID rid,
                                Transaction *transaction);
  [[nodiscard]] Status OnDelete(std::string_view table_name, const Row &row, RID rid);
  [[nodiscard]] Status OnDelete(std::string_view table_name, const Row &row, RID rid,
                                Transaction *transaction);
  [[nodiscard]] Status OnUpdate(std::string_view table_name, const Row &old_row,
                                RID old_rid, const Row &new_row, RID new_rid);
  [[nodiscard]] Status OnUpdate(std::string_view table_name, const Row &old_row,
                                RID old_rid, const Row &new_row, RID new_rid,
                                Transaction *transaction);

  [[nodiscard]] Result<std::vector<RID>> Lookup(std::string_view index_name,
                                                index_key_t key) const;
  // 事务版查询会让索引页面的 S 锁保留到事务结束；短期 PageGuard 仍按
  // 父子锁耦合及时释放，两种锁的生命周期不可混用。
  [[nodiscard]] Result<std::vector<RID>> Lookup(std::string_view index_name,
                                                index_key_t key,
                                                Transaction *transaction) const;
  [[nodiscard]] Result<std::vector<std::pair<index_key_t, RID>>> RangeScan(
      std::string_view index_name, index_key_t lower, index_key_t upper) const;
  [[nodiscard]] Result<std::vector<std::pair<index_key_t, RID>>> RangeScan(
      std::string_view index_name, index_key_t lower, index_key_t upper,
      Transaction *transaction) const;

 private:
  [[nodiscard]] Result<std::unique_ptr<BPlusTree>> OpenTree(
      const IndexMetadata &metadata, Transaction *transaction = nullptr) const;
  [[nodiscard]] Result<std::size_t> IndexedColumn(const IndexMetadata &metadata) const;
  [[nodiscard]] Status PersistRootIfChanged(const IndexMetadata &metadata,
                                            const BPlusTree &tree,
                                            page_id_t old_root,
                                            Transaction *transaction);
  [[nodiscard]] Status LockUniqueKey(const IndexMetadata &metadata,
                                     index_key_t key,
                                     Transaction *transaction) const;

  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
  LockManager *lock_manager_{nullptr};
};

}  // namespace oursql
