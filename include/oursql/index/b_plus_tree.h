#pragma once

#include "oursql/index/b_plus_tree_page.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace oursql {

class BPlusTree {
 public:
  explicit BPlusTree(BufferPoolManager *buffer_pool,
                     page_id_t root_page_id = INVALID_PAGE_ID,
                     std::uint32_t leaf_max_size = BPlusTreeLeafPage::kPhysicalMaxSize,
                     std::uint32_t internal_max_size = BPlusTreeInternalPage::kPhysicalMaxSize)
      noexcept;

  // 持久化入口：CreatePersistent 创建索引元数据页，OpenPersistent 仅凭该页编号恢复树。
  [[nodiscard]] static Result<std::unique_ptr<BPlusTree>> CreatePersistent(
      BufferPoolManager *buffer_pool,
      std::uint32_t leaf_max_size = BPlusTreeLeafPage::kPhysicalMaxSize,
      std::uint32_t internal_max_size = BPlusTreeInternalPage::kPhysicalMaxSize);
  [[nodiscard]] static Result<std::unique_ptr<BPlusTree>> OpenPersistent(
      BufferPoolManager *buffer_pool, page_id_t header_page_id);

  [[nodiscard]] bool IsEmpty() const noexcept;
  [[nodiscard]] page_id_t GetRootPageId() const noexcept;
  [[nodiscard]] page_id_t GetHeaderPageId() const noexcept;

  // 相同 Key 可对应多个 RID；完全相同的 Key/RID 对只保存一次。
  [[nodiscard]] Status Insert(index_key_t key, RID rid);
  [[nodiscard]] Status Remove(index_key_t key, RID rid);
  [[nodiscard]] Result<std::vector<RID>> GetValue(index_key_t key) const;
  // 闭区间扫描 [lower, upper]，结果按 Key、RID 排序。
  [[nodiscard]] Result<std::vector<std::pair<index_key_t, RID>>> RangeScan(
      index_key_t lower, index_key_t upper) const;

  // 调用者：DROP INDEX；作用：释放元数据页及全部内部页、叶子页；成功后树为空。
  [[nodiscard]] Status Destroy();

 private:
  struct SplitResult {
    index_key_t separator{0};
    page_id_t right_page_id{INVALID_PAGE_ID};
  };

  [[nodiscard]] Result<page_id_t> FindLeaf(index_key_t key,
                                           std::vector<page_id_t> *path) const;
  [[nodiscard]] Result<SplitResult> InsertIntoLeaf(page_id_t leaf_page_id,
                                                   index_key_t key, RID rid);
  [[nodiscard]] Result<std::optional<SplitResult>> InsertIntoInternal(
      page_id_t page_id, page_id_t left_child, index_key_t separator,
      page_id_t right_child);
  [[nodiscard]] Status CreateNewRoot(page_id_t left_child, index_key_t separator,
                                     page_id_t right_child);
  [[nodiscard]] Status SetParent(page_id_t child_page_id, page_id_t parent_page_id);
  [[nodiscard]] Status PersistRootPageId();
  [[nodiscard]] Status RebalanceLeaf(page_id_t leaf_page_id);
  [[nodiscard]] Status RebalanceInternal(page_id_t page_id);
  [[nodiscard]] Status PropagateMinimumChange(page_id_t page_id, index_key_t new_minimum);
  [[nodiscard]] Result<index_key_t> SubtreeMinimum(page_id_t page_id) const;

  BufferPoolManager *buffer_pool_{nullptr};
  page_id_t root_page_id_{INVALID_PAGE_ID};
  page_id_t header_page_id_{INVALID_PAGE_ID};
  std::uint32_t leaf_max_size_{BPlusTreeLeafPage::kPhysicalMaxSize};
  std::uint32_t internal_max_size_{BPlusTreeInternalPage::kPhysicalMaxSize};
};

}  // namespace oursql
