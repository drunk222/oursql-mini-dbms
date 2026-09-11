#pragma once

#include "oursql/catalog/catalog.h"
#include "oursql/index/b_plus_tree.h"
#include "oursql/storage/row_codec.h"

#include <memory>
#include <string_view>
#include <vector>

namespace oursql {

// IndexManager 连接 Catalog、HeapTable 与 B+树；不参与 SQL 解析、Planner 或存储实现。
class IndexManager {
 public:
  IndexManager(Catalog *catalog, BufferPoolManager *buffer_pool) noexcept
      : catalog_(catalog), buffer_pool_(buffer_pool) {}

  [[nodiscard]] Status CreateIndex(std::string_view index_name,
                                   std::string_view table_name,
                                   std::string_view column_name, bool is_unique);
  [[nodiscard]] Status DropIndex(std::string_view index_name);

  [[nodiscard]] Status InsertEntry(std::string_view index_name, index_key_t key, RID rid);
  [[nodiscard]] Status DeleteEntry(std::string_view index_name, index_key_t key, RID rid);
  [[nodiscard]] Status UpdateEntry(std::string_view index_name, index_key_t old_key,
                                   RID old_rid, index_key_t new_key, RID new_rid);

  // 行级维护接口供 INSERT/DELETE/UPDATE 执行路径在后续接线时调用。
  [[nodiscard]] Status OnInsert(std::string_view table_name, const Row &row, RID rid);
  [[nodiscard]] Status OnDelete(std::string_view table_name, const Row &row, RID rid);
  [[nodiscard]] Status OnUpdate(std::string_view table_name, const Row &old_row,
                                RID old_rid, const Row &new_row, RID new_rid);

  [[nodiscard]] Result<std::vector<RID>> Lookup(std::string_view index_name,
                                                index_key_t key) const;
  [[nodiscard]] Result<std::vector<std::pair<index_key_t, RID>>> RangeScan(
      std::string_view index_name, index_key_t lower, index_key_t upper) const;

 private:
  [[nodiscard]] Result<std::unique_ptr<BPlusTree>> OpenTree(
      const IndexMetadata &metadata) const;
  [[nodiscard]] Result<std::size_t> IndexedColumn(const IndexMetadata &metadata) const;
  [[nodiscard]] Status PersistRootIfChanged(const IndexMetadata &metadata,
                                            const BPlusTree &tree,
                                            page_id_t old_root);

  Catalog *catalog_{nullptr};
  BufferPoolManager *buffer_pool_{nullptr};
};

}  // namespace oursql
