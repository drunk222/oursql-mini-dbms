#include "oursql/index/b_plus_tree.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace oursql {
namespace {

constexpr std::uint32_t kHeaderPageType = 0x42504844;  // "BPHD"
constexpr std::uint32_t kHeaderFormatVersion = 1;
constexpr std::size_t kHeaderTypeOffset = 0;
constexpr std::size_t kHeaderVersionOffset = 4;
constexpr std::size_t kHeaderRootOffset = 8;
constexpr std::size_t kHeaderLeafMaxOffset = 12;
constexpr std::size_t kHeaderInternalMaxOffset = 16;

template <typename T>
T LoadHeaderValue(const std::byte *data, std::size_t offset) noexcept {
  T value{};
  std::memcpy(&value, data + offset, sizeof(T));
  return value;
}

template <typename T>
void StoreHeaderValue(std::byte *data, std::size_t offset, T value) noexcept {
  std::memcpy(data + offset, &value, sizeof(T));
}

bool RidLess(const RID &left, const RID &right) noexcept {
  return left.page_id < right.page_id ||
         (left.page_id == right.page_id && left.slot_id < right.slot_id);
}

bool EntryLess(const std::pair<index_key_t, RID> &left,
               const std::pair<index_key_t, RID> &right) noexcept {
  return left.first < right.first ||
         (left.first == right.first && RidLess(left.second, right.second));
}

}  // namespace

BPlusTree::BPlusTree(BufferPoolManager *buffer_pool, page_id_t root_page_id,
                     std::uint32_t leaf_max_size,
                     std::uint32_t internal_max_size,
                     Transaction *transaction) noexcept
    : buffer_pool_(buffer_pool),
      transaction_(transaction),
      root_page_id_(root_page_id),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {}

Result<std::unique_ptr<BPlusTree>> BPlusTree::CreatePersistent(
    BufferPoolManager *buffer_pool, std::uint32_t leaf_max_size,
    std::uint32_t internal_max_size, Transaction *transaction) {
  if (buffer_pool == nullptr) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InvalidArgument("持久化B+树缺少BufferPoolManager"));
  }
  if (leaf_max_size < 2 || leaf_max_size > BPlusTreeLeafPage::kPhysicalMaxSize ||
      internal_max_size < 3 ||
      internal_max_size > BPlusTreeInternalPage::kPhysicalMaxSize) {
    return Result<std::unique_ptr<BPlusTree>>(Status::InvalidArgument("B+树节点容量配置非法"));
  }

  auto header_result = buffer_pool->NewPage(transaction);
  if (!header_result.ok()) {
    return Result<std::unique_ptr<BPlusTree>>(header_result.status());
  }
  auto &guard = header_result.value();
  std::memset(guard.Data(), 0, Page::kSize);
  StoreHeaderValue(guard.Data(), kHeaderTypeOffset, kHeaderPageType);
  StoreHeaderValue(guard.Data(), kHeaderVersionOffset, kHeaderFormatVersion);
  StoreHeaderValue(guard.Data(), kHeaderRootOffset, INVALID_PAGE_ID);
  StoreHeaderValue(guard.Data(), kHeaderLeafMaxOffset, leaf_max_size);
  StoreHeaderValue(guard.Data(), kHeaderInternalMaxOffset, internal_max_size);
  auto status = guard.MarkDirty();
  if (!status.ok()) return Result<std::unique_ptr<BPlusTree>>(status);

  auto tree = std::make_unique<BPlusTree>(buffer_pool, INVALID_PAGE_ID, leaf_max_size,
                                          internal_max_size, transaction);
  tree->header_page_id_ = guard.PageId();
  return Result<std::unique_ptr<BPlusTree>>(std::move(tree));
}

Result<std::unique_ptr<BPlusTree>> BPlusTree::OpenPersistent(
    BufferPoolManager *buffer_pool, page_id_t header_page_id,
    Transaction *transaction) {
  if (buffer_pool == nullptr || header_page_id == INVALID_PAGE_ID) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InvalidArgument("打开持久化B+树需要有效的缓冲池和元数据页编号"));
  }
  auto header_result = buffer_pool->FetchPage(header_page_id);
  if (!header_result.ok()) {
    return Result<std::unique_ptr<BPlusTree>>(header_result.status());
  }
  const auto *data = header_result.value().Data();
  if (LoadHeaderValue<std::uint32_t>(data, kHeaderTypeOffset) != kHeaderPageType ||
      LoadHeaderValue<std::uint32_t>(data, kHeaderVersionOffset) != kHeaderFormatVersion) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InternalError("B+树元数据页类型或版本无效"));
  }
  const auto root = LoadHeaderValue<page_id_t>(data, kHeaderRootOffset);
  const auto leaf_max = LoadHeaderValue<std::uint32_t>(data, kHeaderLeafMaxOffset);
  const auto internal_max =
      LoadHeaderValue<std::uint32_t>(data, kHeaderInternalMaxOffset);
  if (leaf_max < 2 || leaf_max > BPlusTreeLeafPage::kPhysicalMaxSize ||
      internal_max < 3 || internal_max > BPlusTreeInternalPage::kPhysicalMaxSize) {
    return Result<std::unique_ptr<BPlusTree>>(
        Status::InternalError("B+树元数据页中的节点容量无效"));
  }
  auto tree = std::make_unique<BPlusTree>(buffer_pool, root, leaf_max, internal_max,
                                          transaction);
  tree->header_page_id_ = header_page_id;
  return Result<std::unique_ptr<BPlusTree>>(std::move(tree));
}

bool BPlusTree::IsEmpty() const noexcept { return root_page_id_ == INVALID_PAGE_ID; }

page_id_t BPlusTree::GetRootPageId() const noexcept { return root_page_id_; }

page_id_t BPlusTree::GetHeaderPageId() const noexcept { return header_page_id_; }

Result<page_id_t> BPlusTree::FindLeaf(index_key_t key,
                                      std::vector<page_id_t> *path) const {
  if (buffer_pool_ == nullptr) {
    return Result<page_id_t>(Status::InvalidArgument("B+树缺少BufferPoolManager"));
  }
  if (IsEmpty()) return Result<page_id_t>(Status::NotFound("B+树为空"));

  auto current = root_page_id_;
  while (current != INVALID_PAGE_ID) {
    auto guard_result = buffer_pool_->FetchPage(current);
    if (!guard_result.ok()) return Result<page_id_t>(guard_result.status());
    auto &guard = guard_result.value();

    BPlusTreeLeafPage leaf(guard.Data());
    if (leaf.Validate().ok()) return Result<page_id_t>(current);

    BPlusTreeInternalPage internal(guard.Data());
    auto valid = internal.Validate();
    if (!valid.ok()) return Result<page_id_t>(valid);
    if (path != nullptr) path->push_back(current);
    current = internal.Lookup(key);
  }
  return Result<page_id_t>(Status::InternalError("B+树内部页包含无效子指针"));
}

Status BPlusTree::Insert(index_key_t key, RID rid) {
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("B+树缺少BufferPoolManager");
  if (leaf_max_size_ < 2 || leaf_max_size_ > BPlusTreeLeafPage::kPhysicalMaxSize ||
      internal_max_size_ < 3 ||
      internal_max_size_ > BPlusTreeInternalPage::kPhysicalMaxSize) {
    return Status::InvalidArgument("B+树节点容量配置非法");
  }

  if (IsEmpty()) {
    {
      auto new_page = buffer_pool_->NewPage(transaction_);
      if (!new_page.ok()) return new_page.status();
      auto &guard = new_page.value();
      BPlusTreeLeafPage leaf(guard.Data());
      auto status = leaf.Initialize(leaf_max_size_, INVALID_PAGE_ID);
      if (!status.ok()) return status;
      status = leaf.Insert(key, rid);
      if (!status.ok()) return status;
      status = guard.MarkDirty();
      if (!status.ok()) return status;
      root_page_id_ = guard.PageId();
    }
    return PersistRootPageId();
  }

  std::vector<page_id_t> path;
  auto leaf_result = FindLeaf(key, &path);
  if (!leaf_result.ok()) return leaf_result.status();
  page_id_t left_page_id = leaf_result.value();
  auto split_result = InsertIntoLeaf(left_page_id, key, rid);
  if (!split_result.ok()) return split_result.status();
  auto split = split_result.value();
  if (split.right_page_id == INVALID_PAGE_ID) return Status::Ok();

  while (!path.empty()) {
    const auto parent_page_id = path.back();
    path.pop_back();
    auto parent_result =
        InsertIntoInternal(parent_page_id, left_page_id, split.separator,
                           split.right_page_id);
    if (!parent_result.ok()) return parent_result.status();
    if (!parent_result.value().has_value()) return Status::Ok();
    left_page_id = parent_page_id;
    split = *parent_result.value();
  }
  return CreateNewRoot(left_page_id, split.separator, split.right_page_id);
}

Status BPlusTree::Insert(index_key_t key, RID rid, Transaction *transaction) {
  if (transaction == transaction_) return Insert(key, rid);
  BPlusTree scoped(buffer_pool_, root_page_id_, leaf_max_size_, internal_max_size_, transaction);
  scoped.header_page_id_ = header_page_id_;
  auto status = scoped.Insert(key, rid);
  root_page_id_ = scoped.root_page_id_;
  return status;
}

Result<BPlusTree::SplitResult> BPlusTree::InsertIntoLeaf(page_id_t leaf_page_id,
                                                         index_key_t key, RID rid) {
  auto old_result = buffer_pool_->FetchPageWrite(leaf_page_id, transaction_);
  if (!old_result.ok()) return Result<SplitResult>(old_result.status());
  auto &old_guard = old_result.value();
  BPlusTreeLeafPage old_leaf(old_guard.Data());
  auto valid = old_leaf.Validate();
  if (!valid.ok()) return Result<SplitResult>(valid);

  auto entries = old_leaf.Entries();
  const auto position = std::lower_bound(entries.begin(), entries.end(),
                                         std::make_pair(key, rid), EntryLess);
  if (position != entries.end() && position->first == key && position->second == rid) {
    return Result<SplitResult>(Status::AlreadyExists("B+树中已存在相同的Key/RID"));
  }
  if (entries.size() < old_leaf.GetMaxSize()) {
    auto status = old_leaf.Insert(key, rid);
    if (!status.ok()) return Result<SplitResult>(status);
    status = old_guard.MarkDirty();
    if (!status.ok()) return Result<SplitResult>(status);
    return Result<SplitResult>(SplitResult{});
  }

  entries.insert(position, {key, rid});
  auto new_result = buffer_pool_->NewPage(transaction_);
  if (!new_result.ok()) return Result<SplitResult>(new_result.status());
  auto &new_guard = new_result.value();
  BPlusTreeLeafPage new_leaf(new_guard.Data());
  auto status = new_leaf.Initialize(old_leaf.GetMaxSize(), old_leaf.GetParentPageId(),
                                    old_leaf.GetNextPageId());
  if (!status.ok()) return Result<SplitResult>(status);

  const auto split_at = entries.size() / 2;
  std::vector<std::pair<index_key_t, RID>> left(entries.begin(), entries.begin() + split_at);
  std::vector<std::pair<index_key_t, RID>> right(entries.begin() + split_at, entries.end());
  status = old_leaf.Assign(left);
  if (!status.ok()) return Result<SplitResult>(status);
  status = new_leaf.Assign(right);
  if (!status.ok()) return Result<SplitResult>(status);
  old_leaf.SetNextPageId(new_guard.PageId());
  status = old_guard.MarkDirty();
  if (!status.ok()) return Result<SplitResult>(status);
  status = new_guard.MarkDirty();
  if (!status.ok()) return Result<SplitResult>(status);
  return Result<SplitResult>(SplitResult{right.front().first, new_guard.PageId()});
}

Result<std::optional<BPlusTree::SplitResult>> BPlusTree::InsertIntoInternal(
    page_id_t page_id, page_id_t left_child, index_key_t separator,
    page_id_t right_child) {
  std::vector<page_id_t> moved_children;
  page_id_t new_page_id = INVALID_PAGE_ID;
  index_key_t promoted = 0;
  {
    auto old_result = buffer_pool_->FetchPageWrite(page_id, transaction_);
    if (!old_result.ok()) {
      return Result<std::optional<SplitResult>>(old_result.status());
    }
    auto &old_guard = old_result.value();
    BPlusTreeInternalPage old_page(old_guard.Data());
    auto valid = old_page.Validate();
    if (!valid.ok()) return Result<std::optional<SplitResult>>(valid);
    auto entries = old_page.Entries();
    const auto left = std::find_if(entries.begin(), entries.end(),
                                   [left_child](const auto &entry) {
                                     return entry.second == left_child;
                                   });
    if (left == entries.end()) {
      return Result<std::optional<SplitResult>>(
          Status::InternalError("B+树父页未包含待分裂的子页"));
    }
    entries.insert(left + 1, {separator, right_child});
    if (entries.size() <= old_page.GetMaxSize()) {
      auto status = old_page.Assign(entries);
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
      status = old_guard.MarkDirty();
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
      // 在 guard 释放后更新孩子，避免小缓冲池中同时 pin 过多页面。
    } else {
      auto new_result = buffer_pool_->NewPage(transaction_);
      if (!new_result.ok()) {
        return Result<std::optional<SplitResult>>(new_result.status());
      }
      auto &new_guard = new_result.value();
      new_page_id = new_guard.PageId();
      BPlusTreeInternalPage new_page(new_guard.Data());
      auto status = new_page.Initialize(old_page.GetMaxSize(), old_page.GetParentPageId());
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);

      const auto split_at = entries.size() / 2;
      promoted = entries[split_at].first;
      std::vector<std::pair<index_key_t, page_id_t>> left_entries(
          entries.begin(), entries.begin() + split_at);
      std::vector<std::pair<index_key_t, page_id_t>> right_entries(
          entries.begin() + split_at, entries.end());
      right_entries.front().first = 0;  // 内部页第 0 个 Key 不参与查找。
      for (const auto &entry : right_entries) moved_children.push_back(entry.second);
      status = old_page.Assign(left_entries);
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
      status = new_page.Assign(right_entries);
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
      status = old_guard.MarkDirty();
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
      status = new_guard.MarkDirty();
      if (!status.ok()) return Result<std::optional<SplitResult>>(status);
    }
  }

  if (new_page_id == INVALID_PAGE_ID) {
    auto status = SetParent(right_child, page_id);
    if (!status.ok()) return Result<std::optional<SplitResult>>(status);
    return Result<std::optional<SplitResult>>(std::optional<SplitResult>{});
  }
  for (const auto child : moved_children) {
    auto status = SetParent(child, new_page_id);
    if (!status.ok()) return Result<std::optional<SplitResult>>(status);
  }
  return Result<std::optional<SplitResult>>(
      std::optional<SplitResult>(SplitResult{promoted, new_page_id}));
}

Status BPlusTree::CreateNewRoot(page_id_t left_child, index_key_t separator,
                                page_id_t right_child) {
  page_id_t new_root_id = INVALID_PAGE_ID;
  {
    auto root_result = buffer_pool_->NewPage(transaction_);
    if (!root_result.ok()) return root_result.status();
    auto &guard = root_result.value();
    new_root_id = guard.PageId();
    BPlusTreeInternalPage root(guard.Data());
    auto status = root.Initialize(internal_max_size_, INVALID_PAGE_ID);
    if (!status.ok()) return status;
    status = root.Assign({{0, left_child}, {separator, right_child}});
    if (!status.ok()) return status;
    status = guard.MarkDirty();
    if (!status.ok()) return status;
  }
  auto status = SetParent(left_child, new_root_id);
  if (!status.ok()) return status;
  status = SetParent(right_child, new_root_id);
  if (!status.ok()) return status;
  root_page_id_ = new_root_id;
  return PersistRootPageId();
}

Status BPlusTree::PersistRootPageId() {
  // 非持久化构造方式继续可用于临时索引和旧调用者。
  if (header_page_id_ == INVALID_PAGE_ID) return Status::Ok();
  auto header_result = buffer_pool_->FetchPageWrite(header_page_id_, transaction_);
  if (!header_result.ok()) return header_result.status();
  auto &guard = header_result.value();
  if (LoadHeaderValue<std::uint32_t>(guard.Data(), kHeaderTypeOffset) != kHeaderPageType ||
      LoadHeaderValue<std::uint32_t>(guard.Data(), kHeaderVersionOffset) !=
          kHeaderFormatVersion) {
    return Status::InternalError("B+树元数据页损坏，无法更新根页面编号");
  }
  StoreHeaderValue(guard.Data(), kHeaderRootOffset, root_page_id_);
  return guard.MarkDirty();
}

Status BPlusTree::SetParent(page_id_t child_page_id, page_id_t parent_page_id) {
  auto child_result = buffer_pool_->FetchPageWrite(child_page_id, transaction_);
  if (!child_result.ok()) return child_result.status();
  auto &guard = child_result.value();
  BPlusTreeLeafPage leaf(guard.Data());
  if (leaf.Validate().ok()) {
    leaf.SetParentPageId(parent_page_id);
    return guard.MarkDirty();
  }
  BPlusTreeInternalPage internal(guard.Data());
  auto valid = internal.Validate();
  if (!valid.ok()) return valid;
  internal.SetParentPageId(parent_page_id);
  return guard.MarkDirty();
}

Status BPlusTree::Remove(index_key_t key, RID rid) {
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("B+树缺少BufferPoolManager");
  if (IsEmpty()) return Status::NotFound("B+树为空");

  auto leaf_result = FindLeaf(key, nullptr);
  if (!leaf_result.ok()) return leaf_result.status();
  page_id_t target = INVALID_PAGE_ID;
  auto current = leaf_result.value();
  while (current != INVALID_PAGE_ID && target == INVALID_PAGE_ID) {
    auto guard_result = buffer_pool_->FetchPage(current);
    if (!guard_result.ok()) return guard_result.status();
    BPlusTreeLeafPage leaf(guard_result.value().Data());
    auto valid = leaf.Validate();
    if (!valid.ok()) return valid;
    const auto entries = leaf.Entries();
    const auto found = std::find(entries.begin(), entries.end(), std::make_pair(key, rid));
    if (found != entries.end()) {
      target = current;
      break;
    }
    if (!entries.empty() && entries.back().first > key) break;
    current = leaf.GetNextPageId();
  }
  if (target == INVALID_PAGE_ID) return Status::NotFound("B+树未找到指定的Key/RID");

  bool root_became_empty = false;
  bool minimum_changed = false;
  index_key_t new_minimum = 0;
  std::uint32_t size = 0;
  std::uint32_t max_size = 0;
  {
    auto guard_result = buffer_pool_->FetchPageWrite(target, transaction_);
    if (!guard_result.ok()) return guard_result.status();
    auto &guard = guard_result.value();
    BPlusTreeLeafPage leaf(guard.Data());
    auto entries = leaf.Entries();
    const auto found = std::find(entries.begin(), entries.end(), std::make_pair(key, rid));
    if (found == entries.end()) return Status::NotFound("B+树未找到指定的Key/RID");
    minimum_changed = found == entries.begin();
    entries.erase(found);
    auto status = leaf.Assign(entries);
    if (!status.ok()) return status;
    status = guard.MarkDirty();
    if (!status.ok()) return status;
    size = leaf.GetSize();
    max_size = leaf.GetMaxSize();
    root_became_empty = target == root_page_id_ && entries.empty();
    if (!entries.empty()) new_minimum = entries.front().first;
  }
  if (target == root_page_id_) {
    if (!root_became_empty) return Status::Ok();
    root_page_id_ = INVALID_PAGE_ID;
    auto status = PersistRootPageId();
    if (!status.ok()) return status;
    return buffer_pool_->DeletePage(target, transaction_);
  }
  const auto minimum_size = (max_size + 1U) / 2U;
  if (size >= minimum_size) {
    return minimum_changed ? PropagateMinimumChange(target, new_minimum) : Status::Ok();
  }
  return RebalanceLeaf(target);
}

Status BPlusTree::PropagateMinimumChange(page_id_t page_id, index_key_t new_minimum) {
  auto current = page_id;
  while (true) {
    page_id_t parent_id = INVALID_PAGE_ID;
    {
      auto page_result = buffer_pool_->FetchPage(current);
      if (!page_result.ok()) return page_result.status();
      BPlusTreeLeafPage leaf(page_result.value().Data());
      if (leaf.Validate().ok()) {
        parent_id = leaf.GetParentPageId();
      } else {
        BPlusTreeInternalPage internal(page_result.value().Data());
        auto valid = internal.Validate();
        if (!valid.ok()) return valid;
        parent_id = internal.GetParentPageId();
      }
    }
    if (parent_id == INVALID_PAGE_ID) return Status::Ok();
    auto parent_result = buffer_pool_->FetchPageWrite(parent_id, transaction_);
    if (!parent_result.ok()) return parent_result.status();
    auto &guard = parent_result.value();
    BPlusTreeInternalPage parent(guard.Data());
    auto entries = parent.Entries();
    const auto child = std::find_if(entries.begin(), entries.end(), [current](const auto &entry) {
      return entry.second == current;
    });
    if (child == entries.end()) return Status::InternalError("B+树父页缺少子页面");
    const auto index = static_cast<std::size_t>(child - entries.begin());
    if (index != 0) {
      entries[index].first = new_minimum;
      auto status = parent.Assign(entries);
      if (!status.ok()) return status;
      return guard.MarkDirty();
    }
    current = parent_id;
  }
}

Result<index_key_t> BPlusTree::SubtreeMinimum(page_id_t page_id) const {
  auto current = page_id;
  while (current != INVALID_PAGE_ID) {
    auto guard_result = buffer_pool_->FetchPage(current);
    if (!guard_result.ok()) return Result<index_key_t>(guard_result.status());
    BPlusTreeLeafPage leaf(guard_result.value().Data());
    if (leaf.Validate().ok()) {
      if (leaf.GetSize() == 0) {
        return Result<index_key_t>(Status::InternalError("B+树非根叶子页为空"));
      }
      return Result<index_key_t>(leaf.KeyAt(0));
    }
    BPlusTreeInternalPage internal(guard_result.value().Data());
    auto valid = internal.Validate();
    if (!valid.ok()) return Result<index_key_t>(valid);
    current = internal.ChildAt(0);
  }
  return Result<index_key_t>(Status::InternalError("B+树子树包含无效页面"));
}

Status BPlusTree::RebalanceLeaf(page_id_t leaf_page_id) {
  page_id_t parent_id = INVALID_PAGE_ID;
  std::uint32_t minimum_size = 0;
  std::vector<std::pair<index_key_t, RID>> leaf_entries;
  page_id_t leaf_next = INVALID_PAGE_ID;
  {
    auto leaf_result = buffer_pool_->FetchPage(leaf_page_id);
    if (!leaf_result.ok()) return leaf_result.status();
    BPlusTreeLeafPage leaf(leaf_result.value().Data());
    auto valid = leaf.Validate();
    if (!valid.ok()) return valid;
    parent_id = leaf.GetParentPageId();
    minimum_size = (leaf.GetMaxSize() + 1U) / 2U;
    leaf_entries = leaf.Entries();
    leaf_next = leaf.GetNextPageId();
  }
  std::vector<std::pair<index_key_t, page_id_t>> parent_entries;
  std::size_t child_index = 0;
  {
    auto parent_result = buffer_pool_->FetchPage(parent_id);
    if (!parent_result.ok()) return parent_result.status();
    BPlusTreeInternalPage parent_read(parent_result.value().Data());
    parent_entries = parent_read.Entries();
    const auto child = std::find_if(parent_entries.begin(), parent_entries.end(),
                                    [leaf_page_id](const auto &entry) {
                                      return entry.second == leaf_page_id;
                                    });
    if (child == parent_entries.end()) return Status::InternalError("B+树父页缺少叶子页面");
    child_index = static_cast<std::size_t>(child - parent_entries.begin());
  }
  const auto left_id = child_index == 0 ? INVALID_PAGE_ID : parent_entries[child_index - 1].second;
  const auto right_id = child_index + 1 >= parent_entries.size()
                            ? INVALID_PAGE_ID
                            : parent_entries[child_index + 1].second;

  if (left_id != INVALID_PAGE_ID) {
    std::vector<std::pair<index_key_t, RID>> left_entries;
    {
      auto left_result = buffer_pool_->FetchPage(left_id);
      if (!left_result.ok()) return left_result.status();
      BPlusTreeLeafPage left(left_result.value().Data());
      left_entries = left.Entries();
    }
    if (left_entries.size() > minimum_size) {
      leaf_entries.insert(leaf_entries.begin(), left_entries.back());
      left_entries.pop_back();
      std::sort(leaf_entries.begin(), leaf_entries.end(), EntryLess);
      {
        auto write_result = buffer_pool_->FetchPageWrite(left_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeLeafPage page(write_result.value().Data());
        auto status = page.Assign(left_entries);
        if (!status.ok()) return status;
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      {
        auto write_result = buffer_pool_->FetchPageWrite(leaf_page_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeLeafPage page(write_result.value().Data());
        auto status = page.Assign(leaf_entries);
        if (!status.ok()) return status;
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      parent_entries[child_index].first = leaf_entries.front().first;
      auto write_result = buffer_pool_->FetchPageWrite(parent_id, transaction_);
      if (!write_result.ok()) return write_result.status();
      BPlusTreeInternalPage page(write_result.value().Data());
      auto status = page.Assign(parent_entries);
      if (!status.ok()) return status;
      return write_result.value().MarkDirty();
    }
  }

  if (right_id != INVALID_PAGE_ID) {
    std::vector<std::pair<index_key_t, RID>> right_entries;
    page_id_t right_next = INVALID_PAGE_ID;
    {
      auto right_result = buffer_pool_->FetchPage(right_id);
      if (!right_result.ok()) return right_result.status();
      BPlusTreeLeafPage right(right_result.value().Data());
      right_entries = right.Entries();
      right_next = right.GetNextPageId();
    }
    if (right_entries.size() > minimum_size) {
      leaf_entries.push_back(right_entries.front());
      right_entries.erase(right_entries.begin());
      std::sort(leaf_entries.begin(), leaf_entries.end(), EntryLess);
      {
        auto write_result = buffer_pool_->FetchPageWrite(leaf_page_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeLeafPage page(write_result.value().Data());
        auto status = page.Assign(leaf_entries);
        if (!status.ok()) return status;
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      {
        auto write_result = buffer_pool_->FetchPageWrite(right_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeLeafPage page(write_result.value().Data());
        auto status = page.Assign(right_entries);
        if (!status.ok()) return status;
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      parent_entries[child_index + 1].first = right_entries.front().first;
      auto write_result = buffer_pool_->FetchPageWrite(parent_id, transaction_);
      if (!write_result.ok()) return write_result.status();
      BPlusTreeInternalPage page(write_result.value().Data());
      auto status = page.Assign(parent_entries);
      if (!status.ok()) return status;
      return write_result.value().MarkDirty();
    }

    if (left_id == INVALID_PAGE_ID) {
      leaf_entries.insert(leaf_entries.end(), right_entries.begin(), right_entries.end());
      std::sort(leaf_entries.begin(), leaf_entries.end(), EntryLess);
      {
        auto write_result = buffer_pool_->FetchPageWrite(leaf_page_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeLeafPage page(write_result.value().Data());
        auto status = page.Assign(leaf_entries);
        if (!status.ok()) return status;
        page.SetNextPageId(right_next);
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      parent_entries.erase(parent_entries.begin() + child_index + 1);
      {
        auto write_result = buffer_pool_->FetchPageWrite(parent_id, transaction_);
        if (!write_result.ok()) return write_result.status();
        BPlusTreeInternalPage page(write_result.value().Data());
        auto status = page.Assign(parent_entries);
        if (!status.ok()) return status;
        if (!(status = write_result.value().MarkDirty()).ok()) return status;
      }
      auto status = buffer_pool_->DeletePage(right_id, transaction_);
      if (!status.ok()) return status;
      return RebalanceInternal(parent_id);
    }
  }

  if (left_id == INVALID_PAGE_ID) return Status::InternalError("B+树叶子页没有可合并的兄弟");
  std::vector<std::pair<index_key_t, RID>> left_entries;
  {
    auto left_result = buffer_pool_->FetchPage(left_id);
    if (!left_result.ok()) return left_result.status();
    BPlusTreeLeafPage left(left_result.value().Data());
    left_entries = left.Entries();
  }
  left_entries.insert(left_entries.end(), leaf_entries.begin(), leaf_entries.end());
  std::sort(left_entries.begin(), left_entries.end(), EntryLess);
  {
    auto write_result = buffer_pool_->FetchPageWrite(left_id, transaction_);
    if (!write_result.ok()) return write_result.status();
    BPlusTreeLeafPage page(write_result.value().Data());
    auto status = page.Assign(left_entries);
    if (!status.ok()) return status;
    page.SetNextPageId(leaf_next);
    if (!(status = write_result.value().MarkDirty()).ok()) return status;
  }
  parent_entries.erase(parent_entries.begin() + child_index);
  {
  auto write_result = buffer_pool_->FetchPageWrite(parent_id, transaction_);
    if (!write_result.ok()) return write_result.status();
    BPlusTreeInternalPage page(write_result.value().Data());
    auto status = page.Assign(parent_entries);
    if (!status.ok()) return status;
    if (!(status = write_result.value().MarkDirty()).ok()) return status;
  }
  auto status = buffer_pool_->DeletePage(leaf_page_id, transaction_);
  if (!status.ok()) return status;
  return RebalanceInternal(parent_id);
}

Status BPlusTree::RebalanceInternal(page_id_t page_id) {
  std::vector<std::pair<index_key_t, page_id_t>> entries;
  page_id_t parent_id = INVALID_PAGE_ID;
  std::uint32_t max_size = 0;
  {
    auto page_result = buffer_pool_->FetchPage(page_id);
    if (!page_result.ok()) return page_result.status();
    BPlusTreeInternalPage page(page_result.value().Data());
    auto valid = page.Validate();
    if (!valid.ok()) return valid;
    entries = page.Entries();
    parent_id = page.GetParentPageId();
    max_size = page.GetMaxSize();
  }
  if (page_id == root_page_id_) {
    if (entries.size() >= 2) return Status::Ok();
    if (entries.empty()) return Status::InternalError("B+树根内部页没有子页面");
    const auto new_root = entries.front().second;
    auto status = SetParent(new_root, INVALID_PAGE_ID);
    if (!status.ok()) return status;
    root_page_id_ = new_root;
    status = PersistRootPageId();
    if (!status.ok()) return status;
    return buffer_pool_->DeletePage(page_id, transaction_);
  }
  const auto minimum_size = (max_size + 1U) / 2U;
  if (entries.size() >= minimum_size) return Status::Ok();

  std::vector<std::pair<index_key_t, page_id_t>> parent_entries;
  std::size_t child_index = 0;
  {
    auto parent_result = buffer_pool_->FetchPage(parent_id);
    if (!parent_result.ok()) return parent_result.status();
    BPlusTreeInternalPage parent_read(parent_result.value().Data());
    parent_entries = parent_read.Entries();
    const auto child = std::find_if(parent_entries.begin(), parent_entries.end(),
                                    [page_id](const auto &entry) {
                                      return entry.second == page_id;
                                    });
    if (child == parent_entries.end()) return Status::InternalError("B+树父页缺少内部页面");
    child_index = static_cast<std::size_t>(child - parent_entries.begin());
  }
  const auto left_id = child_index == 0 ? INVALID_PAGE_ID : parent_entries[child_index - 1].second;
  const auto right_id = child_index + 1 >= parent_entries.size()
                            ? INVALID_PAGE_ID
                            : parent_entries[child_index + 1].second;

  if (left_id != INVALID_PAGE_ID) {
    std::vector<std::pair<index_key_t, page_id_t>> left_entries;
    {
      auto result = buffer_pool_->FetchPage(left_id);
      if (!result.ok()) return result.status();
      BPlusTreeInternalPage page(result.value().Data());
      left_entries = page.Entries();
    }
    if (left_entries.size() > minimum_size) {
      auto borrowed = left_entries.back();
      left_entries.pop_back();
      entries.front().first = parent_entries[child_index].first;
      borrowed.first = 0;
      entries.insert(entries.begin(), borrowed);
      parent_entries[child_index].first = left_entries.empty() ? 0 : borrowed.first;
      auto minimum = SubtreeMinimum(borrowed.second);
      if (!minimum.ok()) return minimum.status();
      parent_entries[child_index].first = minimum.value();
      for (const auto &target : std::vector<std::pair<page_id_t, std::vector<std::pair<index_key_t, page_id_t>>>>{
               {left_id, left_entries}, {page_id, entries}, {parent_id, parent_entries}}) {
        auto result = buffer_pool_->FetchPageWrite(target.first, transaction_);
        if (!result.ok()) return result.status();
        BPlusTreeInternalPage page(result.value().Data());
        auto status = page.Assign(target.second);
        if (!status.ok()) return status;
        if (!(status = result.value().MarkDirty()).ok()) return status;
      }
      return SetParent(borrowed.second, page_id);
    }
  }

  if (right_id != INVALID_PAGE_ID) {
    std::vector<std::pair<index_key_t, page_id_t>> right_entries;
    {
      auto result = buffer_pool_->FetchPage(right_id);
      if (!result.ok()) return result.status();
      BPlusTreeInternalPage page(result.value().Data());
      right_entries = page.Entries();
    }
    if (right_entries.size() > minimum_size) {
      auto borrowed = right_entries.front();
      borrowed.first = parent_entries[child_index + 1].first;
      entries.push_back(borrowed);
      right_entries.erase(right_entries.begin());
      auto new_minimum = SubtreeMinimum(right_entries.front().second);
      if (!new_minimum.ok()) return new_minimum.status();
      right_entries.front().first = 0;
      parent_entries[child_index + 1].first = new_minimum.value();
      for (const auto &target : std::vector<std::pair<page_id_t, std::vector<std::pair<index_key_t, page_id_t>>>>{
               {page_id, entries}, {right_id, right_entries}, {parent_id, parent_entries}}) {
        auto result = buffer_pool_->FetchPageWrite(target.first, transaction_);
        if (!result.ok()) return result.status();
        BPlusTreeInternalPage page(result.value().Data());
        auto status = page.Assign(target.second);
        if (!status.ok()) return status;
        if (!(status = result.value().MarkDirty()).ok()) return status;
      }
      return SetParent(borrowed.second, page_id);
    }

    if (left_id == INVALID_PAGE_ID) {
      right_entries.front().first = parent_entries[child_index + 1].first;
      std::vector<page_id_t> moved;
      for (const auto &entry : right_entries) moved.push_back(entry.second);
      entries.insert(entries.end(), right_entries.begin(), right_entries.end());
      parent_entries.erase(parent_entries.begin() + child_index + 1);
      for (const auto &target : std::vector<std::pair<page_id_t, std::vector<std::pair<index_key_t, page_id_t>>>>{
               {page_id, entries}, {parent_id, parent_entries}}) {
        auto result = buffer_pool_->FetchPageWrite(target.first, transaction_);
        if (!result.ok()) return result.status();
        BPlusTreeInternalPage page(result.value().Data());
        auto status = page.Assign(target.second);
        if (!status.ok()) return status;
        if (!(status = result.value().MarkDirty()).ok()) return status;
      }
      auto status = buffer_pool_->DeletePage(right_id, transaction_);
      if (!status.ok()) return status;
      for (const auto moved_child : moved) {
        if (!(status = SetParent(moved_child, page_id)).ok()) return status;
      }
      return RebalanceInternal(parent_id);
    }
  }

  if (left_id == INVALID_PAGE_ID) return Status::InternalError("B+树内部页没有可合并的兄弟");
  std::vector<std::pair<index_key_t, page_id_t>> left_entries;
  {
    auto result = buffer_pool_->FetchPage(left_id);
    if (!result.ok()) return result.status();
    BPlusTreeInternalPage page(result.value().Data());
    left_entries = page.Entries();
  }
  entries.front().first = parent_entries[child_index].first;
  std::vector<page_id_t> moved;
  for (const auto &entry : entries) moved.push_back(entry.second);
  left_entries.insert(left_entries.end(), entries.begin(), entries.end());
  parent_entries.erase(parent_entries.begin() + child_index);
  for (const auto &target : std::vector<std::pair<page_id_t, std::vector<std::pair<index_key_t, page_id_t>>>>{
           {left_id, left_entries}, {parent_id, parent_entries}}) {
    auto result = buffer_pool_->FetchPageWrite(target.first, transaction_);
    if (!result.ok()) return result.status();
    BPlusTreeInternalPage page(result.value().Data());
    auto status = page.Assign(target.second);
    if (!status.ok()) return status;
    if (!(status = result.value().MarkDirty()).ok()) return status;
  }
  auto status = buffer_pool_->DeletePage(page_id, transaction_);
  if (!status.ok()) return status;
  for (const auto moved_child : moved) {
    if (!(status = SetParent(moved_child, left_id)).ok()) return status;
  }
  return RebalanceInternal(parent_id);
}

Status BPlusTree::Destroy() {
  if (buffer_pool_ == nullptr) return Status::InvalidArgument("B+树缺少BufferPoolManager");
  std::vector<page_id_t> pending;
  std::vector<page_id_t> pages;
  if (root_page_id_ != INVALID_PAGE_ID) pending.push_back(root_page_id_);
  while (!pending.empty()) {
    const auto page_id = pending.back();
    pending.pop_back();
    auto result = buffer_pool_->FetchPage(page_id);
    if (!result.ok()) return result.status();
    BPlusTreeLeafPage leaf(result.value().Data());
    if (!leaf.Validate().ok()) {
      BPlusTreeInternalPage internal(result.value().Data());
      auto valid = internal.Validate();
      if (!valid.ok()) return valid;
      for (const auto &entry : internal.Entries()) pending.push_back(entry.second);
    }
    pages.push_back(page_id);
  }
  root_page_id_ = INVALID_PAGE_ID;
  for (auto it = pages.rbegin(); it != pages.rend(); ++it) {
    auto status = buffer_pool_->DeletePage(*it, transaction_);
    if (!status.ok()) return status;
  }
  if (header_page_id_ != INVALID_PAGE_ID) {
    const auto header = header_page_id_;
    header_page_id_ = INVALID_PAGE_ID;
    return buffer_pool_->DeletePage(header, transaction_);
  }
  return Status::Ok();
}

Status BPlusTree::Remove(index_key_t key, RID rid, Transaction *transaction) {
  if (transaction == transaction_) return Remove(key, rid);
  BPlusTree scoped(buffer_pool_, root_page_id_, leaf_max_size_, internal_max_size_, transaction);
  scoped.header_page_id_ = header_page_id_;
  auto status = scoped.Remove(key, rid);
  root_page_id_ = scoped.root_page_id_;
  return status;
}

Result<std::vector<RID>> BPlusTree::GetValue(index_key_t key) const {
  auto range = RangeScan(key, key);
  if (!range.ok()) return Result<std::vector<RID>>(range.status());
  std::vector<RID> values;
  values.reserve(range.value().size());
  for (const auto &entry : range.value()) values.push_back(entry.second);
  return Result<std::vector<RID>>(std::move(values));
}

Result<std::vector<std::pair<index_key_t, RID>>> BPlusTree::RangeScan(
    index_key_t lower, index_key_t upper) const {
  if (lower > upper) {
    return Result<std::vector<std::pair<index_key_t, RID>>>(
        Status::InvalidArgument("B+树范围查询下界不能大于上界"));
  }
  std::vector<std::pair<index_key_t, RID>> result;
  if (IsEmpty()) return Result<std::vector<std::pair<index_key_t, RID>>>(std::move(result));
  auto leaf_result = FindLeaf(lower, nullptr);
  if (!leaf_result.ok()) {
    return Result<std::vector<std::pair<index_key_t, RID>>>(leaf_result.status());
  }
  auto current = leaf_result.value();
  while (current != INVALID_PAGE_ID) {
    auto guard_result = buffer_pool_->FetchPage(current);
    if (!guard_result.ok()) {
      return Result<std::vector<std::pair<index_key_t, RID>>>(guard_result.status());
    }
    BPlusTreeLeafPage leaf(guard_result.value().Data());
    auto valid = leaf.Validate();
    if (!valid.ok()) {
      return Result<std::vector<std::pair<index_key_t, RID>>>(valid);
    }
    for (const auto &entry : leaf.Entries()) {
      if (entry.first < lower) continue;
      if (entry.first > upper) {
        std::sort(result.begin(), result.end(), EntryLess);
        return Result<std::vector<std::pair<index_key_t, RID>>>(std::move(result));
      }
      result.push_back(entry);
    }
    current = leaf.GetNextPageId();
  }
  std::sort(result.begin(), result.end(), EntryLess);
  return Result<std::vector<std::pair<index_key_t, RID>>>(std::move(result));
}

}  // namespace oursql
