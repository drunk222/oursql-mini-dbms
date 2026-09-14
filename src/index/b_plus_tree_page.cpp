#include "oursql/index/b_plus_tree_page.h"

#include <algorithm>
#include <cstring>

namespace oursql {
namespace {

constexpr std::size_t kTypeOffset = 0;
constexpr std::size_t kSizeOffset = 4;
constexpr std::size_t kMaxSizeOffset = 8;
constexpr std::size_t kParentOffset = 12;
constexpr std::size_t kNextOffset = 16;

template <typename T>
T Load(const std::byte *data, std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, data + offset, sizeof(T));
  return value;
}

template <typename T>
void Store(std::byte *data, std::size_t offset, T value) noexcept {
  std::memcpy(data + offset, &value, sizeof(T));
}

bool RidLess(const RID &left, const RID &right) noexcept {
  return left.page_id < right.page_id ||
         (left.page_id == right.page_id && left.slot_id < right.slot_id);
}

}  // namespace

Status BPlusTreeLeafPage::Initialize(std::uint32_t max_size, page_id_t parent_page_id,
                                     page_id_t next_page_id) {
  if (data_ == nullptr || max_size < 2 || max_size > kPhysicalMaxSize) {
    return Status::InvalidArgument("B+树叶子页容量必须在 2 到物理上限之间");
  }
  std::memset(data_, 0, Page::kSize);
  Store(data_, kTypeOffset, kPageType);
  Store(data_, kSizeOffset, std::uint32_t{0});
  Store(data_, kMaxSizeOffset, max_size);
  Store(data_, kParentOffset, parent_page_id);
  Store(data_, kNextOffset, next_page_id);
  return Status::Ok();
}

Status BPlusTreeLeafPage::Validate() const {
  if (data_ == nullptr || Load<std::uint32_t>(data_, kTypeOffset) != kPageType) {
    return Status::InternalError("页面不是有效的B+树叶子页");
  }
  const auto max_size = GetMaxSize();
  if (max_size < 2 || max_size > kPhysicalMaxSize || GetSize() > max_size) {
    return Status::InternalError("B+树叶子页头损坏");
  }
  for (std::uint32_t i = 1; i < GetSize(); ++i) {
    if (KeyAt(i) < KeyAt(i - 1) ||
        (KeyAt(i) == KeyAt(i - 1) && RidLess(ValueAt(i), ValueAt(i - 1)))) {
      return Status::InternalError("B+树叶子页键值无序");
    }
  }
  return Status::Ok();
}

std::uint32_t BPlusTreeLeafPage::GetSize() const noexcept {
  return Load<std::uint32_t>(data_, kSizeOffset);
}
std::uint32_t BPlusTreeLeafPage::GetMaxSize() const noexcept {
  return Load<std::uint32_t>(data_, kMaxSizeOffset);
}
page_id_t BPlusTreeLeafPage::GetParentPageId() const noexcept {
  return Load<page_id_t>(data_, kParentOffset);
}
page_id_t BPlusTreeLeafPage::GetNextPageId() const noexcept {
  return Load<page_id_t>(data_, kNextOffset);
}
void BPlusTreeLeafPage::SetParentPageId(page_id_t page_id) noexcept {
  Store(data_, kParentOffset, page_id);
}
void BPlusTreeLeafPage::SetNextPageId(page_id_t page_id) noexcept {
  Store(data_, kNextOffset, page_id);
}

index_key_t BPlusTreeLeafPage::KeyAt(std::uint32_t index) const noexcept {
  return Load<index_key_t>(data_, kHeaderSize + index * kEntrySize);
}
RID BPlusTreeLeafPage::ValueAt(std::uint32_t index) const noexcept {
  const auto offset = kHeaderSize + index * kEntrySize + sizeof(index_key_t);
  return RID{Load<page_id_t>(data_, offset), Load<slot_id_t>(data_, offset + 4)};
}
std::uint32_t BPlusTreeLeafPage::LowerBound(index_key_t key) const noexcept {
  std::uint32_t first = 0;
  std::uint32_t count = GetSize();
  while (count != 0) {
    const auto step = count / 2;
    const auto middle = first + step;
    if (KeyAt(middle) < key) {
      first = middle + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  return first;
}

Status BPlusTreeLeafPage::Insert(index_key_t key, RID rid) {
  auto entries = Entries();
  const auto position = std::lower_bound(
      entries.begin(), entries.end(), std::make_pair(key, rid),
      [](const auto &left, const auto &right) {
        return left.first < right.first ||
               (left.first == right.first && RidLess(left.second, right.second));
      });
  if (position != entries.end() && position->first == key && position->second == rid) {
    return Status::AlreadyExists("B+树中已存在相同的Key/RID");
  }
  if (entries.size() >= GetMaxSize()) return Status::OutOfSpace("B+树叶子页已满");
  entries.insert(position, {key, rid});
  return Assign(entries);
}
 
std::vector<std::pair<index_key_t, RID>> BPlusTreeLeafPage::Entries() const {
  std::vector<std::pair<index_key_t, RID>> entries;
  entries.reserve(GetSize());
  for (std::uint32_t i = 0; i < GetSize(); ++i) entries.emplace_back(KeyAt(i), ValueAt(i));
  return entries;
}

Status BPlusTreeLeafPage::Assign(
    const std::vector<std::pair<index_key_t, RID>> &entries) {
  if (entries.size() > GetMaxSize()) return Status::OutOfSpace("B+树叶子页赋值溢出");
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto offset = kHeaderSize + i * kEntrySize;
    Store(data_, offset, entries[i].first);
    Store(data_, offset + sizeof(index_key_t), entries[i].second.page_id);
    Store(data_, offset + sizeof(index_key_t) + 4, entries[i].second.slot_id);
  }
  Store(data_, kSizeOffset, static_cast<std::uint32_t>(entries.size()));
  return Status::Ok();
}

Status BPlusTreeInternalPage::Initialize(std::uint32_t max_size,
                                         page_id_t parent_page_id) {
  if (data_ == nullptr || max_size < 3 || max_size > kPhysicalMaxSize) {
    return Status::InvalidArgument("B+树内部页容量必须在 3 到物理上限之间");
  }
  std::memset(data_, 0, Page::kSize);
  Store(data_, kTypeOffset, kPageType);
  Store(data_, kSizeOffset, std::uint32_t{0});
  Store(data_, kMaxSizeOffset, max_size);
  Store(data_, kParentOffset, parent_page_id);
  return Status::Ok();
}

Status BPlusTreeInternalPage::Validate() const {
  if (data_ == nullptr || Load<std::uint32_t>(data_, kTypeOffset) != kPageType) {
    return Status::InternalError("页面不是有效的B+树内部页");
  }
  const auto max_size = GetMaxSize();
  if (max_size < 3 || max_size > kPhysicalMaxSize || GetSize() > max_size) {
    return Status::InternalError("B+树内部页头损坏");
  }
  for (std::uint32_t i = 2; i < GetSize(); ++i) {
    if (KeyAt(i) < KeyAt(i - 1)) return Status::InternalError("B+树内部页键值无序");
  }
  return Status::Ok();
}

std::uint32_t BPlusTreeInternalPage::GetSize() const noexcept {
  return Load<std::uint32_t>(data_, kSizeOffset);
}
std::uint32_t BPlusTreeInternalPage::GetMaxSize() const noexcept {
  return Load<std::uint32_t>(data_, kMaxSizeOffset);
}
page_id_t BPlusTreeInternalPage::GetParentPageId() const noexcept {
  return Load<page_id_t>(data_, kParentOffset);
}
void BPlusTreeInternalPage::SetParentPageId(page_id_t page_id) noexcept {
  Store(data_, kParentOffset, page_id);
}
index_key_t BPlusTreeInternalPage::KeyAt(std::uint32_t index) const noexcept {
  return Load<index_key_t>(data_, kHeaderSize + index * kEntrySize);
}
page_id_t BPlusTreeInternalPage::ChildAt(std::uint32_t index) const noexcept {
  return Load<page_id_t>(data_, kHeaderSize + index * kEntrySize + sizeof(index_key_t));
}
page_id_t BPlusTreeInternalPage::Lookup(index_key_t key) const noexcept {
  if (GetSize() == 0) return INVALID_PAGE_ID;
  std::uint32_t child = 0;
  while (child + 1 < GetSize() && KeyAt(child + 1) < key) ++child;
  return ChildAt(child);
}
std::vector<std::pair<index_key_t, page_id_t>> BPlusTreeInternalPage::Entries() const {
  std::vector<std::pair<index_key_t, page_id_t>> entries;
  entries.reserve(GetSize());
  for (std::uint32_t i = 0; i < GetSize(); ++i) entries.emplace_back(KeyAt(i), ChildAt(i));
  return entries;
}
Status BPlusTreeInternalPage::Assign(
    const std::vector<std::pair<index_key_t, page_id_t>> &entries) {
  if (entries.size() > GetMaxSize()) return Status::OutOfSpace("B+树内部页赋值溢出");
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto offset = kHeaderSize + i * kEntrySize;
    Store(data_, offset, entries[i].first);
    Store(data_, offset + sizeof(index_key_t), entries[i].second);
  }
  Store(data_, kSizeOffset, static_cast<std::uint32_t>(entries.size()));
  return Status::Ok();
}

}  // namespace oursql
