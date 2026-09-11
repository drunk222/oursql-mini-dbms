#pragma once

#include "oursql/common/status.h"
#include "oursql/common/types.h"
#include "oursql/storage/storage.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace oursql {

using index_key_t = std::int64_t;
static_assert(Page::kSize == 4096, "B+树磁盘页格式要求Page固定为4096字节");

// B+ 树页面使用独立的定长布局，不复用 SlottedPage，避免索引页与数据页格式耦合。
class BPlusTreeLeafPage {
 public:
  static constexpr std::uint32_t kPageType = 0x42504C46;  // "BPLF"
  static constexpr std::size_t kHeaderSize = 32;
  static constexpr std::size_t kEntrySize = 16;
  static constexpr std::uint32_t kPhysicalMaxSize =
      static_cast<std::uint32_t>((Page::kSize - kHeaderSize) / kEntrySize);

  explicit BPlusTreeLeafPage(std::byte *data) noexcept : data_(data) {}
  explicit BPlusTreeLeafPage(const std::byte *data) noexcept
      : data_(const_cast<std::byte *>(data)) {}

  [[nodiscard]] Status Initialize(std::uint32_t max_size, page_id_t parent_page_id,
                                  page_id_t next_page_id = INVALID_PAGE_ID);
  [[nodiscard]] Status Validate() const;
  [[nodiscard]] std::uint32_t GetSize() const noexcept;
  [[nodiscard]] std::uint32_t GetMaxSize() const noexcept;
  [[nodiscard]] page_id_t GetParentPageId() const noexcept;
  [[nodiscard]] page_id_t GetNextPageId() const noexcept;
  void SetParentPageId(page_id_t page_id) noexcept;
  void SetNextPageId(page_id_t page_id) noexcept;

  [[nodiscard]] index_key_t KeyAt(std::uint32_t index) const noexcept;
  [[nodiscard]] RID ValueAt(std::uint32_t index) const noexcept;
  [[nodiscard]] std::uint32_t LowerBound(index_key_t key) const noexcept;
  [[nodiscard]] Status Insert(index_key_t key, RID rid);
  [[nodiscard]] std::vector<std::pair<index_key_t, RID>> Entries() const;
  [[nodiscard]] Status Assign(const std::vector<std::pair<index_key_t, RID>> &entries);

 private:
  std::byte *data_{nullptr};
};

class BPlusTreeInternalPage {
 public:
  static constexpr std::uint32_t kPageType = 0x4250494E;  // "BPIN"
  static constexpr std::size_t kHeaderSize = 32;
  static constexpr std::size_t kEntrySize = 16;
  static constexpr std::uint32_t kPhysicalMaxSize =
      static_cast<std::uint32_t>((Page::kSize - kHeaderSize) / kEntrySize);

  explicit BPlusTreeInternalPage(std::byte *data) noexcept : data_(data) {}
  explicit BPlusTreeInternalPage(const std::byte *data) noexcept
      : data_(const_cast<std::byte *>(data)) {}

  [[nodiscard]] Status Initialize(std::uint32_t max_size, page_id_t parent_page_id);
  [[nodiscard]] Status Validate() const;
  [[nodiscard]] std::uint32_t GetSize() const noexcept;
  [[nodiscard]] std::uint32_t GetMaxSize() const noexcept;
  [[nodiscard]] page_id_t GetParentPageId() const noexcept;
  void SetParentPageId(page_id_t page_id) noexcept;

  // size 表示子指针个数；KeyAt(0) 未使用，KeyAt(i) 是 ChildAt(i) 的下界。
  [[nodiscard]] index_key_t KeyAt(std::uint32_t index) const noexcept;
  [[nodiscard]] page_id_t ChildAt(std::uint32_t index) const noexcept;
  [[nodiscard]] page_id_t Lookup(index_key_t key) const noexcept;
  [[nodiscard]] std::vector<std::pair<index_key_t, page_id_t>> Entries() const;
  [[nodiscard]] Status Assign(
      const std::vector<std::pair<index_key_t, page_id_t>> &entries);

 private:
  std::byte *data_{nullptr};
};

}  // namespace oursql
