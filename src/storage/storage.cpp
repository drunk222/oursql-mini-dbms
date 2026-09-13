#include "oursql/storage/storage.h"
#include "oursql/storage/log_manager.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

namespace oursql {

namespace {

constexpr std::size_t kSuperblockMagicSize = 8;

constexpr std::array<std::byte, kSuperblockMagicSize> kMagic{
    std::byte{'O'}, std::byte{'U'}, std::byte{'R'}, std::byte{'S'},
    std::byte{'Q'}, std::byte{'L'}, std::byte{0}, std::byte{0}};

constexpr std::size_t kMagicOffset = 0;
constexpr std::size_t kFormatVersionOffset = 8;
constexpr std::size_t kPageSizeOffset = 12;
constexpr std::size_t kPageCountOffset = 16;
constexpr std::size_t kFreeListHeadOffset = 20;
constexpr std::size_t kCatalogHeadOffset = 24;
constexpr std::size_t kSuperblockHeaderSize = 28;

std::uint32_t LoadU32FileLittleEndian(const std::byte *data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

void StoreU32FileLittleEndian(std::byte *data, std::size_t offset, std::uint32_t value) {
  data[offset] = std::byte{static_cast<unsigned char>(value & 0xFFU)};
  data[offset + 1] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)};
  data[offset + 2] = std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)};
  data[offset + 3] = std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)};
}

page_id_t LoadPageId(const std::byte *data, std::size_t offset) {
  return static_cast<page_id_t>(LoadU32FileLittleEndian(data, offset));
}

void StorePageId(std::byte *data, std::size_t offset, page_id_t value) {
  StoreU32FileLittleEndian(data, offset, static_cast<std::uint32_t>(value));
}

bool SameMagic(const std::byte *data) {
  return std::memcmp(data + kMagicOffset, kMagic.data(), kMagic.size()) == 0;
}

std::string PathToUtf8(const std::filesystem::path &path) {
#ifdef _WIN32
  return path.u8string();
#else
  return path.string();
#endif
}

}  // namespace

DiskManager::DiskManager(std::filesystem::path file_path) {
  (void)Open(file_path);
}

DiskManager::~DiskManager() {
  (void)Close();
}

Status DiskManager::Open(const std::filesystem::path &file_path) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (is_open_) {
    auto close_status = CloseUnlocked();
    if (!close_status.ok()) {
      return close_status;
    }
  }

  std::error_code ec;
  const bool exists = std::filesystem::exists(file_path, ec);
  if (ec) {
    return Status::IOError("检查数据库文件是否存在失败: " + PathToUtf8(file_path));
  }

  if (!exists) {
    return OpenNewFileUnlocked(file_path);
  }

  const auto size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    return Status::IOError("读取数据库文件大小失败: " + PathToUtf8(file_path));
  }

  if (size == 0) {
    return OpenNewFileUnlocked(file_path);
  }

  if (size % Page::kSize != 0) {
    return Status::IOError("数据库文件大小不是 4096 字节的整数倍: " + PathToUtf8(file_path));
  }

  return OpenExistingFileUnlocked(file_path);
}

Status DiskManager::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  return CloseUnlocked();
}

Result<Page> DiskManager::ReadPage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return ReadPhysicalPageUnlocked(page_id);
}

Status DiskManager::WritePage(page_id_t page_id, const Page &page) {
  std::lock_guard<std::mutex> lock(mutex_);
  return WritePhysicalPageUnlocked(page_id, page);
}

Result<page_id_t> DiskManager::AllocatePage() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (!is_open_) {
    return Result<page_id_t>(Status::InvalidArgument("DiskManager 尚未打开"));
  }

  if (free_list_head_ != INVALID_PAGE_ID) {
    auto page_id = PopFreePageUnlocked();
    if (!page_id.ok()) {
      return page_id;
    }
    return page_id;
  }

  const page_id_t new_page_id = page_count_;
  const page_id_t new_page_count = page_count_ + 1;
  const auto extend_status = EnsureFileSizeUnlocked(new_page_count);
  if (!extend_status.ok()) {
    return Result<page_id_t>(extend_status);
  }

  const page_id_t old_page_count = page_count_;
  page_count_ = new_page_count;
  const auto persist_status = PersistSuperblockUnlocked();
  if (!persist_status.ok()) {
    page_count_ = old_page_count;
    return Result<page_id_t>(persist_status);
  }

  return Result<page_id_t>(new_page_id);
}

Status DiskManager::DeallocatePage(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return PushFreePageUnlocked(page_id);
}

std::uint64_t DiskManager::GetReadCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return read_count_;
}

std::uint64_t DiskManager::GetWriteCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return write_count_;
}

void DiskManager::ResetIoStatistics() {
  std::lock_guard<std::mutex> lock(mutex_);
  read_count_ = 0;
  write_count_ = 0;
}

page_id_t DiskManager::GetCatalogHead() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return catalog_head_;
}

Status DiskManager::SetCatalogHead(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!is_open_) {
    return Status::InvalidArgument("DiskManager 尚未打开");
  }
  if (page_id != INVALID_PAGE_ID && (page_id == 0 || page_id >= page_count_)) {
    return Status::InvalidArgument("目录头页面编号非法");
  }
  const page_id_t old_catalog_head = catalog_head_;
  catalog_head_ = page_id;
  const auto persist_status = PersistSuperblockUnlocked();
  if (!persist_status.ok()) {
    catalog_head_ = old_catalog_head;
  }
  return persist_status;
}

Status DiskManager::CloseUnlocked() {
  if (!is_open_) {
    return Status::Ok();
  }

  if (file_.is_open()) {
    file_.flush();
    file_.close();
    if (!file_) {
      // Keep the logical state consistent when the stream reports a close error.
      is_open_ = false;
      page_count_ = 1;
      free_list_head_ = INVALID_PAGE_ID;
      catalog_head_ = INVALID_PAGE_ID;
      free_pages_.clear();
      return Status::IOError("关闭数据库文件失败: " + PathToUtf8(file_path_));
    }
  }

  is_open_ = false;
  file_path_.clear();
  page_count_ = 1;
  free_list_head_ = INVALID_PAGE_ID;
  catalog_head_ = INVALID_PAGE_ID;
  free_pages_.clear();
  return Status::Ok();
}

Status DiskManager::OpenNewFileUnlocked(const std::filesystem::path &file_path) {
  file_.open(file_path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  if (!file_.is_open()) {
    return Status::IOError("创建数据库文件失败: " + PathToUtf8(file_path));
  }

  file_path_ = file_path;
  page_count_ = 1;
  free_list_head_ = INVALID_PAGE_ID;
  catalog_head_ = INVALID_PAGE_ID;
  free_pages_.clear();
  is_open_ = true;

  const auto persist_status = PersistSuperblockUnlocked();
  if (!persist_status.ok()) {
    CloseUnlocked();
  }
  return persist_status;
}

Status DiskManager::OpenExistingFileUnlocked(const std::filesystem::path &file_path) {
  file_.open(file_path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file_.is_open()) {
    return Status::IOError("打开数据库文件失败: " + PathToUtf8(file_path));
  }

  file_path_ = file_path;
  is_open_ = true;

  Page superblock;
  auto read_status = ReadPhysicalPageUnlocked(0);
  if (!read_status.ok()) {
    CloseUnlocked();
    return read_status.status();
  }
  superblock = std::move(read_status.value());

  if (!SameMagic(superblock.Data())) {
    CloseUnlocked();
    return Status::InvalidArgument("数据库文件魔数不匹配");
  }

  const auto format_version = LoadU32FileLittleEndian(superblock.Data(), kFormatVersionOffset);
  if (format_version != kFormatVersion) {
    CloseUnlocked();
    return Status::NotImplemented("不支持的数据库文件格式版本");
  }

  const auto page_size = LoadU32FileLittleEndian(superblock.Data(), kPageSizeOffset);
  if (page_size != Page::kSize) {
    CloseUnlocked();
    return Status::InvalidArgument("数据库文件页面大小不匹配");
  }

  page_count_ = LoadPageId(superblock.Data(), kPageCountOffset);
  free_list_head_ = LoadPageId(superblock.Data(), kFreeListHeadOffset);
  catalog_head_ = LoadPageId(superblock.Data(), kCatalogHeadOffset);
  free_pages_.clear();

  if (page_count_ == 0) {
    CloseUnlocked();
    return Status::InvalidArgument("数据库文件页数非法");
  }

  std::error_code ec;
  const auto size = std::filesystem::file_size(file_path_, ec);
  if (ec) {
    CloseUnlocked();
    return Status::IOError("读取数据库文件大小失败: " + PathToUtf8(file_path_));
  }
  if (size != static_cast<std::uint64_t>(page_count_) * Page::kSize) {
    CloseUnlocked();
    return Status::IOError("数据库文件大小与页数不一致");
  }

  auto validate_status = ValidateMetadataUnlocked();
  if (!validate_status.ok()) {
    CloseUnlocked();
    return validate_status;
  }

  return Status::Ok();
}

Status DiskManager::PersistSuperblockUnlocked() {
  if (!is_open_) {
    return Status::InvalidArgument("DiskManager 尚未打开");
  }

  Page superblock;
  auto *data = superblock.Data();
  std::memcpy(data + kMagicOffset, kMagic.data(), kMagic.size());
  StoreU32FileLittleEndian(data, kFormatVersionOffset, kFormatVersion);
  StoreU32FileLittleEndian(data, kPageSizeOffset, static_cast<std::uint32_t>(Page::kSize));
  StorePageId(data, kPageCountOffset, page_count_);
  StorePageId(data, kFreeListHeadOffset, free_list_head_);
  StorePageId(data, kCatalogHeadOffset, catalog_head_);
  return WritePhysicalPageUnlocked(0, superblock);
}

Result<Page> DiskManager::ReadPhysicalPageUnlocked(page_id_t page_id) {
  if (!is_open_) {
    return Result<Page>(Status::InvalidArgument("DiskManager 尚未打开"));
  }
  if (page_id >= page_count_) {
    return Result<Page>(Status::NotFound("页面不存在: " + std::to_string(page_id)));
  }

  Page page;
  file_.clear();
  const auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(Page::kSize);
  file_.seekg(offset, std::ios::beg);
  if (!file_) {
    return Result<Page>(Status::IOError("定位读取位置失败: " + std::to_string(page_id)));
  }

  file_.read(reinterpret_cast<char *>(page.Data()), static_cast<std::streamsize>(Page::kSize));
  const auto bytes_read = static_cast<std::size_t>(file_.gcount());
  if (bytes_read != Page::kSize) {
    return Result<Page>(Status::IOError("短读页面: " + std::to_string(page_id)));
  }

  ++read_count_;
  return Result<Page>(std::move(page));
}

Status DiskManager::WritePhysicalPageUnlocked(page_id_t page_id, const Page &page) {
  if (!is_open_) {
    return Status::InvalidArgument("DiskManager 尚未打开");
  }
  if (page_id >= page_count_) {
    return Status::NotFound("页面不存在: " + std::to_string(page_id));
  }

  file_.clear();
  const auto offset = static_cast<std::streamoff>(page_id) * static_cast<std::streamoff>(Page::kSize);
  file_.seekp(offset, std::ios::beg);
  if (!file_) {
    return Status::IOError("定位写入位置失败: " + std::to_string(page_id));
  }

  file_.write(reinterpret_cast<const char *>(page.Data()), static_cast<std::streamsize>(Page::kSize));
  file_.flush();
  if (!file_) {
    return Status::IOError("写入页面失败: " + std::to_string(page_id));
  }

  ++write_count_;
  return Status::Ok();
}

Status DiskManager::EnsureFileSizeUnlocked(std::uint64_t page_count) {
  const auto desired_size = page_count * Page::kSize;
  std::error_code ec;
  const auto current_size = std::filesystem::file_size(file_path_, ec);
  if (ec) {
    return Status::IOError("读取数据库文件大小失败: " + PathToUtf8(file_path_));
  }

  if (current_size >= desired_size) {
    return Status::Ok();
  }

  file_.clear();
  file_.seekp(static_cast<std::streamoff>(desired_size - 1), std::ios::beg);
  if (!file_) {
    return Status::IOError("扩展数据库文件失败: " + PathToUtf8(file_path_));
  }

  const char zero = 0;
  file_.write(&zero, 1);
  file_.flush();
  if (!file_) {
    return Status::IOError("扩展数据库文件失败: " + PathToUtf8(file_path_));
  }

  return Status::Ok();
}

Result<page_id_t> DiskManager::PopFreePageUnlocked() {
  if (free_list_head_ == INVALID_PAGE_ID) {
    return Result<page_id_t>(Status::NotFound("没有可复用的空闲页"));
  }
  if (free_pages_.find(free_list_head_) == free_pages_.end()) {
    return Result<page_id_t>(Status::IOError("空闲页链表与内存状态不一致"));
  }

  const page_id_t page_id = free_list_head_;
  auto page = ReadPhysicalPageUnlocked(page_id);
  if (!page.ok()) {
    return Result<page_id_t>(page.status());
  }

  const page_id_t next_free = LoadPageId(page.value().Data(), 0);
  if (next_free != INVALID_PAGE_ID && next_free >= page_count_) {
    return Result<page_id_t>(Status::IOError("空闲页链表损坏"));
  }
  if (next_free != INVALID_PAGE_ID && next_free == 0) {
    return Result<page_id_t>(Status::IOError("空闲页链表指向非法页面"));
  }

  const page_id_t old_free_list_head = free_list_head_;
  auto old_free_pages = free_pages_;
  free_list_head_ = next_free;
  auto persist_status = PersistSuperblockUnlocked();
  if (!persist_status.ok()) {
    free_list_head_ = old_free_list_head;
    free_pages_ = std::move(old_free_pages);
    return Result<page_id_t>(persist_status);
  }

  free_pages_.erase(page_id);
  return Result<page_id_t>(page_id);
}

Status DiskManager::PushFreePageUnlocked(page_id_t page_id) {
  if (!is_open_) {
    return Status::InvalidArgument("DiskManager 尚未打开");
  }
  if (page_id == 0 || page_id == INVALID_PAGE_ID || page_id >= page_count_) {
    return Status::InvalidArgument("释放的页面编号非法");
  }
  if (free_pages_.find(page_id) != free_pages_.end()) {
    return Status::AlreadyExists("页面已经释放: " + std::to_string(page_id));
  }

  Page free_page;
  StorePageId(free_page.Data(), 0, free_list_head_);
  auto write_status = WritePhysicalPageUnlocked(page_id, free_page);
  if (!write_status.ok()) {
    return write_status;
  }

  const page_id_t old_free_list_head = free_list_head_;
  auto old_free_pages = free_pages_;
  free_list_head_ = page_id;
  auto persist_status = PersistSuperblockUnlocked();
  if (!persist_status.ok()) {
    free_list_head_ = old_free_list_head;
    free_pages_ = std::move(old_free_pages);
    return persist_status;
  }

  free_pages_.insert(page_id);
  return Status::Ok();
}

Status DiskManager::ValidateMetadataUnlocked() {
  if (free_list_head_ != INVALID_PAGE_ID && (free_list_head_ == 0 || free_list_head_ >= page_count_)) {
    return Status::IOError("空闲页头非法");
  }
  if (catalog_head_ != INVALID_PAGE_ID && (catalog_head_ == 0 || catalog_head_ >= page_count_)) {
    return Status::IOError("目录头非法");
  }

  free_pages_.clear();
  page_id_t current = free_list_head_;
  while (current != INVALID_PAGE_ID) {
    if (current == 0 || current >= page_count_) {
      return Status::IOError("空闲页链表越界");
    }
    if (!free_pages_.insert(current).second) {
      return Status::IOError("空闲页链表出现重复或环");
    }

    auto page = ReadPhysicalPageUnlocked(current);
    if (!page.ok()) {
      return page.status();
    }

    current = LoadPageId(page.value().Data(), 0);
  }

  return Status::Ok();
}

FIFOReplacer::FIFOReplacer(std::size_t capacity) : capacity_(capacity) {}

Status FIFOReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("FIFO frame 编号越界: " + std::to_string(frame_id));
  }
  const auto found = candidates_.find(frame_id);
  if (found == candidates_.end()) {
    return Status::Ok();
  }
  candidates_.erase(found);
  order_.remove(frame_id);
  return Status::Ok();
}

Status FIFOReplacer::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("FIFO frame 编号越界: " + std::to_string(frame_id));
  }
  if (candidates_.find(frame_id) != candidates_.end()) {
    return Status::AlreadyExists("FIFO frame 已在候选集合: " + std::to_string(frame_id));
  }
  if (arrival_order_.find(frame_id) == arrival_order_.end()) {
    arrival_order_[frame_id] = next_arrival_++;
  }
  candidates_.insert(frame_id);
  const auto arrival = arrival_order_[frame_id];
  auto position = order_.begin();
  while (position != order_.end() && arrival_order_[*position] <= arrival) ++position;
  order_.insert(position, frame_id);
  return Status::Ok();
}

Status FIFOReplacer::RecordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("FIFO frame 编号越界: " + std::to_string(frame_id));
  }
  if (arrival_order_.find(frame_id) == arrival_order_.end()) {
    arrival_order_[frame_id] = next_arrival_++;
  }
  return Status::Ok();
}

Status FIFOReplacer::Remove(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("FIFO frame 编号越界: " + std::to_string(frame_id));
  }
  candidates_.erase(frame_id);
  order_.remove(frame_id);
  arrival_order_.erase(frame_id);
  return Status::Ok();
}

Result<frame_id_t> FIFOReplacer::Victim() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (order_.empty()) {
    return Result<frame_id_t>(Status::NotFound("FIFO 没有可淘汰的 frame"));
  }
  const frame_id_t frame_id = order_.front();
  order_.pop_front();
  candidates_.erase(frame_id);
  return Result<frame_id_t>(frame_id);
}

std::size_t FIFOReplacer::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return order_.size();
}

bool FIFOReplacer::IsEvictable(frame_id_t frame_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frame_id < capacity_ && candidates_.find(frame_id) != candidates_.end();
}

LRUReplacer::LRUReplacer(std::size_t capacity) : capacity_(capacity) {}

Status LRUReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("LRU frame 编号越界: " + std::to_string(frame_id));
  }
  const auto found = candidates_.find(frame_id);
  if (found == candidates_.end()) {
    last_used_[frame_id] = ++access_clock_;
    return Status::Ok();
  }
  candidates_.erase(found);
  order_.remove(frame_id);
  last_used_[frame_id] = ++access_clock_;
  return Status::Ok();
}

Status LRUReplacer::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("LRU frame 编号越界: " + std::to_string(frame_id));
  }
  if (candidates_.find(frame_id) != candidates_.end()) {
    return Status::AlreadyExists("LRU frame 已在候选集合: " + std::to_string(frame_id));
  }
  if (last_used_.find(frame_id) == last_used_.end()) {
    last_used_[frame_id] = ++access_clock_;
  }
  candidates_.insert(frame_id);
  const auto last_used = last_used_[frame_id];
  auto position = order_.begin();
  while (position != order_.end() && last_used_[*position] <= last_used) ++position;
  order_.insert(position, frame_id);
  return Status::Ok();
}

Status LRUReplacer::RecordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("LRU frame 编号越界: " + std::to_string(frame_id));
  }
  last_used_[frame_id] = ++access_clock_;
  if (candidates_.find(frame_id) != candidates_.end()) {
    candidates_.erase(frame_id);
    order_.remove(frame_id);
    candidates_.insert(frame_id);
    const auto last_used = last_used_[frame_id];
    auto position = order_.begin();
    while (position != order_.end() && last_used_[*position] <= last_used) ++position;
    order_.insert(position, frame_id);
  }
  return Status::Ok();
}

Status LRUReplacer::Remove(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("LRU frame 编号越界: " + std::to_string(frame_id));
  }
  candidates_.erase(frame_id);
  order_.remove(frame_id);
  last_used_.erase(frame_id);
  return Status::Ok();
}

Result<frame_id_t> LRUReplacer::Victim() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (order_.empty()) {
    return Result<frame_id_t>(Status::NotFound("LRU 没有可淘汰的 frame"));
  }
  const frame_id_t frame_id = order_.front();
  order_.pop_front();
  candidates_.erase(frame_id);
  return Result<frame_id_t>(frame_id);
}

std::size_t LRUReplacer::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return order_.size();
}

bool LRUReplacer::IsEvictable(frame_id_t frame_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frame_id < capacity_ && candidates_.find(frame_id) != candidates_.end();
}

ClockReplacer::ClockReplacer(std::size_t capacity)
    : capacity_(capacity),
      active_(capacity, false),
      evictable_(capacity, false),
      reference_bits_(capacity, false) {}

Status ClockReplacer::Pin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("CLOCK frame 编号越界: " + std::to_string(frame_id));
  }
  active_[frame_id] = true;
  reference_bits_[frame_id] = true;
  if (evictable_[frame_id]) {
    evictable_[frame_id] = false;
    --evictable_count_;
  }
  return Status::Ok();
}

Status ClockReplacer::Unpin(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("CLOCK frame 编号越界: " + std::to_string(frame_id));
  }
  active_[frame_id] = true;
  if (evictable_[frame_id]) {
    return Status::AlreadyExists("CLOCK frame 已在候选集合: " + std::to_string(frame_id));
  }
  evictable_[frame_id] = true;
  reference_bits_[frame_id] = true;
  ++evictable_count_;
  return Status::Ok();
}

Status ClockReplacer::RecordAccess(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("CLOCK frame 编号越界: " + std::to_string(frame_id));
  }
  active_[frame_id] = true;
  reference_bits_[frame_id] = true;
  return Status::Ok();
}

Status ClockReplacer::Remove(frame_id_t frame_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (frame_id >= capacity_) {
    return Status::InvalidArgument("CLOCK frame 编号越界: " + std::to_string(frame_id));
  }
  if (evictable_[frame_id]) {
    evictable_[frame_id] = false;
    --evictable_count_;
  }
  active_[frame_id] = false;
  reference_bits_[frame_id] = false;
  return Status::Ok();
}

Result<frame_id_t> ClockReplacer::Victim() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (evictable_count_ == 0 || capacity_ == 0) {
    return Result<frame_id_t>(Status::NotFound("CLOCK 没有可淘汰的 frame"));
  }
  const std::size_t max_steps = capacity_ * 2;
  for (std::size_t step = 0; step < max_steps; ++step) {
    const frame_id_t frame_id = hand_;
    hand_ = (hand_ + 1) % capacity_;
    if (!active_[frame_id] || !evictable_[frame_id]) {
      continue;
    }
    if (reference_bits_[frame_id]) {
      reference_bits_[frame_id] = false;
      continue;
    }
    evictable_[frame_id] = false;
    --evictable_count_;
    active_[frame_id] = false;
    return Result<frame_id_t>(frame_id);
  }
  return Result<frame_id_t>(Status::NotFound("CLOCK 扫描后没有可淘汰的 frame"));
}

std::size_t ClockReplacer::Size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return evictable_count_;
}

bool ClockReplacer::IsEvictable(frame_id_t frame_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frame_id < capacity_ && evictable_[frame_id];
}

ReadPageGuard::ReadPageGuard(BufferPoolManager *buffer_pool, frame_id_t frame_id,
                             page_id_t page_id, std::shared_mutex *latch)
    : buffer_pool_(buffer_pool), frame_id_(frame_id), page_id_(page_id), latch_(*latch) {}

ReadPageGuard::~ReadPageGuard() { Reset(); }

ReadPageGuard::ReadPageGuard(ReadPageGuard &&other) noexcept
    : buffer_pool_(other.buffer_pool_),
      frame_id_(other.frame_id_),
      page_id_(other.page_id_),
      latch_(std::move(other.latch_)) {
  other.buffer_pool_ = nullptr;
  other.page_id_ = INVALID_PAGE_ID;
}

ReadPageGuard &ReadPageGuard::operator=(ReadPageGuard &&other) noexcept {
  if (this != &other) {
    Reset();
    buffer_pool_ = other.buffer_pool_;
    frame_id_ = other.frame_id_;
    page_id_ = other.page_id_;
    latch_ = std::move(other.latch_);
    other.buffer_pool_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
  }
  return *this;
}

const std::byte *ReadPageGuard::Data() const noexcept {
  if (!IsValid()) {
    return nullptr;
  }
  return buffer_pool_->frames_[frame_id_].page.Data();
}

page_id_t ReadPageGuard::PageId() const noexcept {
  return IsValid() ? page_id_ : INVALID_PAGE_ID;
}

bool ReadPageGuard::IsValid() const noexcept {
  return buffer_pool_ != nullptr && latch_.owns_lock();
}

void ReadPageGuard::Reset() noexcept {
  if (buffer_pool_ == nullptr) {
    return;
  }
  if (latch_.owns_lock()) {
    latch_.unlock();
  }
  (void)buffer_pool_->ReleaseGuard(frame_id_, page_id_, false);
  buffer_pool_ = nullptr;
  page_id_ = INVALID_PAGE_ID;
}

WritePageGuard::WritePageGuard(BufferPoolManager *buffer_pool, frame_id_t frame_id,
                               page_id_t page_id, std::shared_mutex *latch,
                               Transaction *transaction)
    : buffer_pool_(buffer_pool),
      frame_id_(frame_id),
      page_id_(page_id),
      transaction_(transaction),
      latch_(*latch) {
  if (transaction_ != nullptr && IsValid()) {
    std::copy(buffer_pool_->frames_[frame_id_].page.Data(),
              buffer_pool_->frames_[frame_id_].page.Data() + Page::kSize,
              before_image_.begin());
    has_before_image_ = true;
    transaction_->AddActiveWriteGuard();
  }
}

WritePageGuard::~WritePageGuard() { (void)Reset(); }

WritePageGuard::WritePageGuard(WritePageGuard &&other) noexcept
    : buffer_pool_(other.buffer_pool_),
      frame_id_(other.frame_id_),
      page_id_(other.page_id_),
      dirty_(other.dirty_),
      transaction_(other.transaction_),
      before_image_(other.before_image_),
      has_before_image_(other.has_before_image_),
      latch_(std::move(other.latch_)) {
  other.buffer_pool_ = nullptr;
  other.page_id_ = INVALID_PAGE_ID;
  other.dirty_ = false;
  other.transaction_ = nullptr;
  other.has_before_image_ = false;
}

WritePageGuard &WritePageGuard::operator=(WritePageGuard &&other) noexcept {
  if (this != &other) {
    (void)Reset();
    buffer_pool_ = other.buffer_pool_;
    frame_id_ = other.frame_id_;
    page_id_ = other.page_id_;
    dirty_ = other.dirty_;
    transaction_ = other.transaction_;
    before_image_ = other.before_image_;
    has_before_image_ = other.has_before_image_;
    latch_ = std::move(other.latch_);
    other.buffer_pool_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.dirty_ = false;
    other.transaction_ = nullptr;
    other.has_before_image_ = false;
  }
  return *this;
}

std::byte *WritePageGuard::Data() noexcept {
  if (!IsValid()) {
    return nullptr;
  }
  return buffer_pool_->frames_[frame_id_].page.Data();
}

const std::byte *WritePageGuard::Data() const noexcept {
  if (!IsValid()) {
    return nullptr;
  }
  return buffer_pool_->frames_[frame_id_].page.Data();
}

page_id_t WritePageGuard::PageId() const noexcept {
  return IsValid() ? page_id_ : INVALID_PAGE_ID;
}

Status WritePageGuard::MarkDirty() noexcept {
  if (!IsValid()) {
    return Status::InvalidArgument("WritePageGuard 已失效");
  }
  dirty_ = true;
  return Status::Ok();
}

bool WritePageGuard::IsValid() const noexcept {
  return buffer_pool_ != nullptr && latch_.owns_lock();
}

Status WritePageGuard::Release() noexcept { return Reset(); }

Status WritePageGuard::Reset() noexcept {
  if (buffer_pool_ == nullptr) {
    return Status::Ok();
  }
  Status log_status = Status::Ok();
  if (latch_.owns_lock() && transaction_ != nullptr && has_before_image_ && dirty_) {
    bool logged = false;
    log_status = buffer_pool_->RecordGuardUpdate(frame_id_, page_id_, transaction_, dirty_,
                                                  before_image_, &logged);
    if (!log_status.ok()) dirty_ = false;
  }
  if (latch_.owns_lock()) {
    latch_.unlock();
  }
  const auto status = buffer_pool_->ReleaseGuard(frame_id_, page_id_, dirty_);
  if (!status.ok() && transaction_ != nullptr) transaction_->MarkFailed();
  buffer_pool_ = nullptr;
  page_id_ = INVALID_PAGE_ID;
  dirty_ = false;
  if (transaction_ != nullptr) transaction_->ReleaseActiveWriteGuard();
  transaction_ = nullptr;
  has_before_image_ = false;
  if (!log_status.ok()) return log_status;
  return status;
}

namespace {

std::uint32_t LoadU32LittleEndian(const std::byte *data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8U) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16U) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

void StoreU32LittleEndian(std::byte *data, std::size_t offset, std::uint32_t value) {
  data[offset] = std::byte{static_cast<unsigned char>(value & 0xFFU)};
  data[offset + 1] = std::byte{static_cast<unsigned char>((value >> 8U) & 0xFFU)};
  data[offset + 2] = std::byte{static_cast<unsigned char>((value >> 16U) & 0xFFU)};
  data[offset + 3] = std::byte{static_cast<unsigned char>((value >> 24U) & 0xFFU)};
}

std::size_t SlotOffset(slot_id_t slot_id, std::size_t field_offset) {
  return SlottedPage::kHeaderSize + static_cast<std::size_t>(slot_id) * SlottedPage::kSlotSize +
         field_offset;
}

std::uint32_t SlotField(const std::byte *data, slot_id_t slot_id, std::size_t field_offset) {
  return LoadU32LittleEndian(data, SlotOffset(slot_id, field_offset));
}

void SetSlotField(std::byte *data, slot_id_t slot_id, std::size_t field_offset, std::uint32_t value) {
  StoreU32LittleEndian(data, SlotOffset(slot_id, field_offset), value);
}

}  // namespace

Status SlottedPage::ValidateBytes(const std::byte *data) {
  if (data == nullptr) {
    return Status::InvalidArgument("Slotted Page 数据为空");
  }
  if (LoadU32LittleEndian(data, kPageTypeOffset) != kPageType) {
    return Status::InvalidArgument("页面类型不是 Slotted Page");
  }
  if (LoadU32LittleEndian(data, kReservedOffset) != 0 ||
      LoadU32LittleEndian(data, kReservedOffset + 4) != 0 ||
      LoadU32LittleEndian(data, kReservedOffset + 8) != 0) {
    return Status::InvalidArgument("Slotted Page 保留字段必须为 0");
  }

  const auto slot_count = LoadU32LittleEndian(data, kSlotCountOffset);
  const auto max_slots = (Page::kSize - kHeaderSize) / kSlotSize;
  if (slot_count > max_slots) {
    return Status::InvalidArgument("Slotted Page slot_count 越界");
  }

  const auto free_start = LoadU32LittleEndian(data, kFreeStartOffset);
  auto free_end = LoadU32LittleEndian(data, kFreeEndOffset);
  const auto expected_free_start = kHeaderSize + static_cast<std::size_t>(slot_count) * kSlotSize;
  if (free_start != expected_free_start || free_start > free_end || free_end > Page::kSize) {
    return Status::InvalidArgument("Slotted Page 空闲区边界非法");
  }

  const auto next_page_id = LoadU32LittleEndian(data, kNextPageIdOffset);
  if (next_page_id == 0) {
    return Status::InvalidArgument("Slotted Page next_page_id 不能指向 Page 0");
  }

  for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
    const auto slot_id = static_cast<slot_id_t>(slot);
    const auto offset = SlotField(data, slot_id, 0);
    const auto length = SlotField(data, slot_id, 4);
    const auto state = SlotField(data, slot_id, 8);
    if (state != kDeletedSlot && state != kUsedSlot) {
      return Status::InvalidArgument("Slotted Page 槽状态非法: " + std::to_string(slot));
    }
    if (state == kDeletedSlot) {
      if (offset != 0 || length != 0) {
        return Status::InvalidArgument("已删除槽必须清空目录项: " + std::to_string(slot));
      }
      continue;
    }
    if (offset < free_start || offset < free_end || offset > Page::kSize ||
        static_cast<std::uint64_t>(offset) + length > Page::kSize) {
      return Status::InvalidArgument("Slotted Page 记录边界非法: " + std::to_string(slot));
    }
    for (std::uint32_t prior = 0; prior < slot; ++prior) {
      if (SlotField(data, static_cast<slot_id_t>(prior), 8) != kUsedSlot) {
        continue;
      }
      const auto prior_offset = SlotField(data, static_cast<slot_id_t>(prior), 0);
      const auto prior_length = SlotField(data, static_cast<slot_id_t>(prior), 4);
      const auto ends_before = static_cast<std::uint64_t>(offset) >=
                               static_cast<std::uint64_t>(prior_offset) + prior_length;
      const auto prior_ends_before = static_cast<std::uint64_t>(prior_offset) >=
                                     static_cast<std::uint64_t>(offset) + length;
      if (!ends_before && !prior_ends_before) {
        return Status::InvalidArgument("Slotted Page 有重叠记录");
      }
    }
  }
  return Status::Ok();
}

Status SlottedPage::Initialize(WritePageGuard &page, page_id_t next_page_id) {
  if (!page.IsValid()) {
    return Status::InvalidArgument("初始化需要有效的 WritePageGuard");
  }
  if (next_page_id == 0) {
    return Status::InvalidArgument("next_page_id 不能指向 Page 0");
  }
  auto *data = page.Data();
  std::fill(data, data + Page::kSize, std::byte{0});
  StoreU32LittleEndian(data, kPageTypeOffset, kPageType);
  StoreU32LittleEndian(data, kSlotCountOffset, 0);
  StoreU32LittleEndian(data, kFreeStartOffset, static_cast<std::uint32_t>(kHeaderSize));
  StoreU32LittleEndian(data, kFreeEndOffset, static_cast<std::uint32_t>(Page::kSize));
  StoreU32LittleEndian(data, kNextPageIdOffset, next_page_id);
  return page.MarkDirty();
}

Status SlottedPage::Validate(const ReadPageGuard &page) {
  if (!page.IsValid()) {
    return Status::InvalidArgument("校验需要有效的 ReadPageGuard");
  }
  return ValidateBytes(page.Data());
}

Result<slot_id_t> SlottedPage::InsertRecord(WritePageGuard &page,
                                            const std::vector<std::byte> &record) {
  if (!page.IsValid()) {
    return Result<slot_id_t>(Status::InvalidArgument("插入需要有效的 WritePageGuard"));
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) {
    return Result<slot_id_t>(valid);
  }
  if (record.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Result<slot_id_t>(Status::InvalidArgument("记录长度超过页面格式上限"));
  }
  if (record.size() > kMaxRecordSize) {
    return Result<slot_id_t>(Status::RecordTooLarge(
        "单条记录超过 Slotted Page 上限: " + std::to_string(kMaxRecordSize)));
  }

  auto *data = page.Data();
  auto slot_count = LoadU32LittleEndian(data, kSlotCountOffset);
  const auto max_slots = (Page::kSize - kHeaderSize) / kSlotSize;
  slot_id_t reusable_slot = INVALID_SLOT_ID;
  for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
    if (SlotField(data, static_cast<slot_id_t>(slot), 8) == kDeletedSlot) {
      reusable_slot = static_cast<slot_id_t>(slot);
      break;
    }
  }
  const auto free_start = LoadU32LittleEndian(data, kFreeStartOffset);
  auto free_end = LoadU32LittleEndian(data, kFreeEndOffset);
  std::uint64_t active_bytes = 0;
  for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
    if (SlotField(data, static_cast<slot_id_t>(slot), 8) == kUsedSlot) {
      active_bytes += SlotField(data, static_cast<slot_id_t>(slot), 4);
    }
  }
  const auto required = static_cast<std::uint64_t>(record.size()) +
                        (reusable_slot == INVALID_SLOT_ID ? kSlotSize : 0);
  const auto total_free = static_cast<std::uint64_t>(Page::kSize) - free_start - active_bytes;
  if (required > total_free) {
    return Result<slot_id_t>(Status::OutOfSpace("页面没有足够的总空间容纳记录"));
  }
  if (reusable_slot == INVALID_SLOT_ID && slot_count >= max_slots) {
    return Result<slot_id_t>(Status::OutOfSpace("槽目录已满"));
  }

  auto has_contiguous_space = [&]() {
    const auto new_free_start = kHeaderSize +
                                (static_cast<std::size_t>(slot_count) +
                                 (reusable_slot == INVALID_SLOT_ID ? 1U : 0U)) *
                                    kSlotSize;
    return new_free_start <= free_end && record.size() <= free_end - new_free_start;
  };
  if (!has_contiguous_space()) {
    auto compact_status = Compact(page);
    if (!compact_status.ok()) {
      return Result<slot_id_t>(compact_status);
    }
    data = page.Data();
    slot_count = LoadU32LittleEndian(data, kSlotCountOffset);
    free_end = LoadU32LittleEndian(data, kFreeEndOffset);
    if (!has_contiguous_space()) {
      return Result<slot_id_t>(Status::OutOfSpace("整理后连续空间仍不足"));
    }
  }

  const auto new_free_start = kHeaderSize +
                              (static_cast<std::size_t>(slot_count) +
                               (reusable_slot == INVALID_SLOT_ID ? 1U : 0U)) *
                                  kSlotSize;
  const auto new_free_end = free_end - static_cast<std::uint32_t>(record.size());
  if (!record.empty()) {
    std::memcpy(data + new_free_end, record.data(), record.size());
  }
  const auto target_slot = reusable_slot == INVALID_SLOT_ID
                               ? static_cast<slot_id_t>(slot_count)
                               : reusable_slot;
  SetSlotField(data, target_slot, 0, new_free_end);
  SetSlotField(data, target_slot, 4,
               static_cast<std::uint32_t>(record.size()));
  SetSlotField(data, target_slot, 8, kUsedSlot);
  if (reusable_slot == INVALID_SLOT_ID) {
    StoreU32LittleEndian(data, kSlotCountOffset, slot_count + 1);
  }
  StoreU32LittleEndian(data, kFreeStartOffset, static_cast<std::uint32_t>(new_free_start));
  StoreU32LittleEndian(data, kFreeEndOffset, new_free_end);
  auto dirty_status = page.MarkDirty();
  if (!dirty_status.ok()) {
    return Result<slot_id_t>(dirty_status);
  }
  return Result<slot_id_t>(target_slot);
}

Result<std::vector<std::byte>> SlottedPage::GetRecord(const ReadPageGuard &page, slot_id_t slot_id) {
  if (!page.IsValid()) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument("读取需要有效的 ReadPageGuard"));
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) {
    return Result<std::vector<std::byte>>(valid);
  }
  const auto slot_count = LoadU32LittleEndian(page.Data(), kSlotCountOffset);
  if (slot_id >= slot_count) {
    return Result<std::vector<std::byte>>(Status::NotFound("slot_id 不存在: " + std::to_string(slot_id)));
  }
  const auto *data = page.Data();
  if (SlotField(data, slot_id, 8) != kUsedSlot) {
    return Result<std::vector<std::byte>>(Status::NotFound("slot_id 已删除: " + std::to_string(slot_id)));
  }
  const auto offset = SlotField(data, slot_id, 0);
  const auto length = SlotField(data, slot_id, 4);
  std::vector<std::byte> record(length);
  if (length != 0) {
    std::memcpy(record.data(), data + offset, length);
  }
  return Result<std::vector<std::byte>>(std::move(record));
}

Result<std::vector<std::byte>> SlottedPage::GetRecord(const WritePageGuard &page,
                                                       slot_id_t slot_id) {
  if (!page.IsValid()) {
    return Result<std::vector<std::byte>>(Status::InvalidArgument("读取需要有效的 WritePageGuard"));
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) return Result<std::vector<std::byte>>(valid);
  const auto slot_count = LoadU32LittleEndian(page.Data(), kSlotCountOffset);
  if (slot_id >= slot_count) {
    return Result<std::vector<std::byte>>(Status::NotFound("slot_id 不存在: " + std::to_string(slot_id)));
  }
  const auto *data = page.Data();
  if (SlotField(data, slot_id, 8) != kUsedSlot) {
    return Result<std::vector<std::byte>>(Status::NotFound("slot_id 已删除: " + std::to_string(slot_id)));
  }
  const auto offset = SlotField(data, slot_id, 0);
  const auto length = SlotField(data, slot_id, 4);
  std::vector<std::byte> record(length);
  if (length != 0) std::memcpy(record.data(), data + offset, length);
  return Result<std::vector<std::byte>>(std::move(record));
}

Status SlottedPage::DeleteRecord(WritePageGuard &page, slot_id_t slot_id) {
  if (!page.IsValid()) {
    return Status::InvalidArgument("删除需要有效的 WritePageGuard");
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) {
    return valid;
  }
  const auto slot_count = LoadU32LittleEndian(page.Data(), kSlotCountOffset);
  if (slot_id >= slot_count) {
    return Status::NotFound("slot_id 不存在: " + std::to_string(slot_id));
  }
  auto *data = page.Data();
  if (SlotField(data, slot_id, 8) != kUsedSlot) {
    return Status::NotFound("slot_id 已删除: " + std::to_string(slot_id));
  }
  SetSlotField(data, slot_id, 0, 0);
  SetSlotField(data, slot_id, 4, 0);
  SetSlotField(data, slot_id, 8, kDeletedSlot);
  return page.MarkDirty();
}

Status SlottedPage::Compact(WritePageGuard &page) {
  if (!page.IsValid()) {
    return Status::InvalidArgument("整理需要有效的 WritePageGuard");
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) {
    return valid;
  }
  std::array<std::byte, Page::kSize> old_data{};
  std::memcpy(old_data.data(), page.Data(), Page::kSize);
  auto *data = page.Data();
  const auto slot_count = LoadU32LittleEndian(old_data.data(), kSlotCountOffset);
  const auto free_start = LoadU32LittleEndian(old_data.data(), kFreeStartOffset);
  std::uint32_t new_free_end = static_cast<std::uint32_t>(Page::kSize);
  for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
    const auto slot_id = static_cast<slot_id_t>(slot);
    if (SlotField(old_data.data(), slot_id, 8) != kUsedSlot) {
      SetSlotField(data, slot_id, 0, 0);
      SetSlotField(data, slot_id, 4, 0);
      SetSlotField(data, slot_id, 8, kDeletedSlot);
      continue;
    }
    const auto offset = SlotField(old_data.data(), slot_id, 0);
    const auto length = SlotField(old_data.data(), slot_id, 4);
    new_free_end -= length;
    if (length != 0) {
      std::memcpy(data + new_free_end, old_data.data() + offset, length);
    }
    SetSlotField(data, slot_id, 0, new_free_end);
    SetSlotField(data, slot_id, 4, length);
    SetSlotField(data, slot_id, 8, kUsedSlot);
  }
  std::fill(data + free_start, data + new_free_end, std::byte{0});
  StoreU32LittleEndian(data, kFreeEndOffset, new_free_end);
  return page.MarkDirty();
}

Result<std::uint32_t> SlottedPage::SlotCount(const ReadPageGuard &page) {
  auto valid = Validate(page);
  if (!valid.ok()) {
    return Result<std::uint32_t>(valid);
  }
  return Result<std::uint32_t>(LoadU32LittleEndian(page.Data(), kSlotCountOffset));
}

Result<std::uint32_t> SlottedPage::SlotCount(const WritePageGuard &page) {
  if (!page.IsValid()) {
    return Result<std::uint32_t>(Status::InvalidArgument("读取 slot_count 需要有效的 WritePageGuard"));
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) return Result<std::uint32_t>(valid);
  return Result<std::uint32_t>(LoadU32LittleEndian(page.Data(), kSlotCountOffset));
}

Result<page_id_t> SlottedPage::NextPageId(const ReadPageGuard &page) {
  auto valid = Validate(page);
  if (!valid.ok()) {
    return Result<page_id_t>(valid);
  }
  return Result<page_id_t>(LoadU32LittleEndian(page.Data(), kNextPageIdOffset));
}

Result<page_id_t> SlottedPage::NextPageId(const WritePageGuard &page) {
  if (!page.IsValid()) {
    return Result<page_id_t>(Status::InvalidArgument("读取 next_page_id 需要有效的 WritePageGuard"));
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) return Result<page_id_t>(valid);
  return Result<page_id_t>(LoadU32LittleEndian(page.Data(), kNextPageIdOffset));
}

Status SlottedPage::SetNextPageId(WritePageGuard &page, page_id_t next_page_id) {
  if (!page.IsValid()) {
    return Status::InvalidArgument("设置 next_page_id 需要有效的 WritePageGuard");
  }
  if (next_page_id == 0) {
    return Status::InvalidArgument("next_page_id 不能指向 Page 0");
  }
  auto valid = ValidateBytes(page.Data());
  if (!valid.ok()) return valid;
  StoreU32LittleEndian(page.Data(), kNextPageIdOffset, next_page_id);
  return page.MarkDirty();
}

BufferPoolManager::BufferPoolManager(std::size_t pool_size, DiskManager *disk_manager,
                                     ReplacementPolicy replacement_policy,
                                     FlushPolicy flush_policy)
    : pool_size_(pool_size),
      disk_manager_(disk_manager),
      replacement_policy_(replacement_policy),
      flush_policy_(flush_policy),
      init_status_(Status::Ok()),
      frames_(pool_size) {
  for (frame_id_t frame_id = 0; frame_id < pool_size_; ++frame_id) {
    free_frames_.push_back(frame_id);
    free_frame_set_.insert(frame_id);
  }
  if (pool_size_ == 0) {
    init_status_ = Status::InvalidArgument("缓冲池容量必须大于 0");
  } else if (disk_manager_ == nullptr) {
    init_status_ = Status::InvalidArgument("BufferPoolManager 需要有效的 DiskManager");
  }

  if (replacement_policy_ == ReplacementPolicy::FIFO) {
    replacer_ = std::make_unique<FIFOReplacer>(pool_size_);
  } else if (replacement_policy_ == ReplacementPolicy::LRU) {
    replacer_ = std::make_unique<LRUReplacer>(pool_size_);
  } else {
    replacer_ = std::make_unique<ClockReplacer>(pool_size_);
  }
}

Status BufferPoolManager::SetLogManager(LogManager *log_manager) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (log_manager == nullptr) return Status::InvalidArgument("BufferPoolManager requires a LogManager");
  if (log_manager_bound_ && log_manager_ != log_manager) {
    return Status::InvalidArgument("BufferPoolManager WAL binding cannot be changed");
  }
  log_manager_ = log_manager;
  log_manager_bound_ = true;
  return Status::Ok();
}

Status BufferPoolManager::RestorePage(page_id_t page_id, const Page &page,
                                      bool write_disk) {
  if (page_id == INVALID_PAGE_ID || page_id == 0) {
    return Status::InvalidArgument("RestorePage requires a data page id");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return ready;
  const auto found = page_table_.find(page_id);
  if (found == page_table_.end()) {
    lock.unlock();
    return write_disk ? disk_manager_->WritePage(page_id, page) : Status::Ok();
  }
  Frame &frame = frames_[found->second];
  if (frame.state != FrameState::Valid || frame.pin_count != 0) {
    return Status::InvalidArgument("RestorePage requires an unpinned valid frame");
  }
  auto pin_status = replacer_->Pin(found->second);
  if (!pin_status.ok()) return pin_status;
  frame.pin_count = 1;
  lock.unlock();

  Status status = Status::Ok();
  {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    if (write_disk) status = disk_manager_->WritePage(page_id, page);
    if (status.ok()) frame.page = page;
  }
  lock.lock();
  if (status.ok()) {
    frame.is_dirty = false;
    frame.page_lsn = kInvalidLsn;
  }
  frame.pin_count = 0;
  (void)replacer_->Unpin(found->second);
  NotifyStateChange();
  return status;
}

Status BufferPoolManager::RecordGuardUpdate(
    frame_id_t frame_id, page_id_t page_id, Transaction *transaction, bool dirty,
    const std::array<std::byte, Page::kSize> &before, bool *logged) noexcept {
  if (logged != nullptr) *logged = false;
  if (!dirty || transaction == nullptr) return Status::Ok();
  if (log_manager_ == nullptr) {
    transaction->MarkFailed();
    return Status::InvalidArgument("transactional page write requires a bound LogManager");
  }

  std::array<std::byte, Page::kSize> after{};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_id >= frames_.size() || frames_[frame_id].page_id != page_id) {
      transaction->MarkFailed();
      return Status::InternalError("WritePageGuard frame changed before WAL append");
    }
    std::copy(frames_[frame_id].page.Data(),
              frames_[frame_id].page.Data() + Page::kSize, after.begin());
  }

  LogRecord record;
  record.type = LogRecordType::PageUpdate;
  record.txn_id = transaction->id();
  record.prev_lsn = transaction->last_lsn();
  record.page_id = page_id;
  record.before_image.assign(before.begin(), before.end());
  record.after_image.assign(after.begin(), after.end());
  auto append = log_manager_->Append(std::move(record));
  if (!append.ok()) {
    std::copy(before.begin(), before.end(), frames_[frame_id].page.Data());
    transaction->MarkFailed();
    return Status::IOError("page WAL append failed: " + append.status().message());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_id >= frames_.size() || frames_[frame_id].page_id != page_id) {
      transaction->MarkFailed();
      return Status::InternalError("WritePageGuard frame changed after WAL append");
    }
    frames_[frame_id].page_lsn = append.value();
  }
  transaction->SetLastLsn(append.value());
  if (logged != nullptr) *logged = true;
  return Status::Ok();
}

Status BufferPoolManager::FlushFrameToDisk(page_id_t page_id, const Page &page,
                                           lsn_t page_lsn) {
  if (log_manager_ != nullptr && page_lsn != kInvalidLsn) {
    auto wal_status = log_manager_->Flush(page_lsn);
    if (!wal_status.ok()) return Status::IOError("WAL-before-data failed: " + wal_status.message());
  }
  if (disk_manager_ == nullptr) return Status::InvalidArgument("BufferPoolManager missing DiskManager");
  return disk_manager_->WritePage(page_id, page);
}

Status BufferPoolManager::AppendPageFreeLog(page_id_t page_id, Transaction *transaction) {
  if (transaction == nullptr || log_manager_ == nullptr) {
    return Status::InvalidArgument("transactional page deletion requires a bound LogManager");
  }
  if (!transaction->IsUsableForWork()) {
    return Status::InvalidArgument("page deletion requires an Active transaction");
  }
  if (std::find(transaction->deferred_free_pages().begin(),
                transaction->deferred_free_pages().end(), page_id) !=
      transaction->deferred_free_pages().end()) {
    return Status::AlreadyExists("page deletion is already logged: " + std::to_string(page_id));
  }
  LogRecord record;
  record.type = LogRecordType::PageFree;
  record.txn_id = transaction->id();
  record.prev_lsn = transaction->last_lsn();
  record.page_id = page_id;
  auto append = log_manager_->Append(std::move(record));
  if (!append.ok()) {
    transaction->MarkFailed();
    return append.status();
  }
  transaction->SetLastLsn(append.value());
  transaction->AddDeferredFreePage(page_id);
  return Status::Ok();
}

BufferPoolManager::~BufferPoolManager() {
  if (!closed_) {
    (void)FlushAllPages();
  }
}

Status BufferPoolManager::EnsureReadyUnlocked() const {
  if (!init_status_.ok()) {
    return init_status_;
  }
  if (closed_) {
    return Status::InvalidArgument("BufferPoolManager 已关闭");
  }
  return Status::Ok();
}

Status BufferPoolManager::ValidateDataPageId(page_id_t page_id) const {
  if (page_id == INVALID_PAGE_ID || page_id == 0) {
    return Status::InvalidArgument("缓冲池不能操作该页面编号: " + std::to_string(page_id));
  }
  return Status::Ok();
}

Result<BufferPoolManager::FrameSelection> BufferPoolManager::SelectFrameUnlocked() {
  if (!free_frames_.empty()) {
    const frame_id_t frame_id = free_frames_.front();
    free_frames_.pop_front();
    free_frame_set_.erase(frame_id);
    if (frame_id >= frames_.size() || frames_[frame_id].page_id != INVALID_PAGE_ID) {
      return Result<FrameSelection>(Status::InternalError("BufferPool 空闲 frame 状态损坏"));
    }
    frames_[frame_id].state = FrameState::Loading;
    return Result<FrameSelection>(FrameSelection{frame_id, true});
  }

  auto victim = replacer_->Victim();
  if (!victim.ok()) {
    return Result<FrameSelection>(Status::NotFound("没有可淘汰的 frame，所有页面都被 pin"));
  }

  const frame_id_t frame_id = victim.value();
  Frame &frame = frames_[frame_id];
  if (frame.pin_count != 0 || frame.page_id == INVALID_PAGE_ID ||
      frame.state != FrameState::Valid) {
    return Result<FrameSelection>(Status::InternalError("替换器返回了无效的 frame"));
  }

  frame.state = FrameState::Evicting;
  ++frame.pin_count;
  return Result<FrameSelection>(FrameSelection{frame_id, false});
}

void BufferPoolManager::RestoreSelectionUnlocked(const FrameSelection &selection) {
  if (selection.from_free_list) {
    if (selection.frame_id < frames_.size()) {
      frames_[selection.frame_id].state = FrameState::Empty;
    }
    (void)ReturnFreeFrameUnlocked(selection.frame_id);
    return;
  }
  if (selection.frame_id < frames_.size() && frames_[selection.frame_id].page_id != INVALID_PAGE_ID &&
      frames_[selection.frame_id].pin_count == 0) {
    (void)replacer_->Unpin(selection.frame_id);
  }
  if (selection.frame_id < frames_.size()) {
    Frame &frame = frames_[selection.frame_id];
    if (frame.state == FrameState::Evicting) {
      frame.state = FrameState::Valid;
      if (frame.pin_count != 0) --frame.pin_count;
      (void)replacer_->Unpin(selection.frame_id);
    }
  }
}

Status BufferPoolManager::ReturnFreeFrameUnlocked(frame_id_t frame_id) {
  if (frame_id >= frames_.size()) {
    return Status::InvalidArgument("归还的 free frame 编号越界: " + std::to_string(frame_id));
  }
  if (frames_[frame_id].page_id != INVALID_PAGE_ID || frames_[frame_id].pin_count != 0) {
    return Status::InternalError("只能归还未占用的 free frame: " + std::to_string(frame_id));
  }
  if (!free_frame_set_.insert(frame_id).second) {
    return Status::InternalError("free frame 重复归还: " + std::to_string(frame_id));
  }
  free_frames_.push_back(frame_id);
  return Status::Ok();
}

void BufferPoolManager::NotifyStateChange() noexcept { state_changed_.notify_all(); }

Status BufferPoolManager::ReleaseGuard(frame_id_t frame_id, page_id_t page_id,
                                       bool is_dirty) noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  if (frame_id >= frames_.size() || frames_[frame_id].page_id != page_id) {
    return Status::InternalError("PageGuard 对应的 frame 已失效");
  }
  Frame &frame = frames_[frame_id];
  if (frame.pin_count == 0) {
    return Status::InvalidArgument("页面 pin_count 已经为 0: " + std::to_string(page_id));
  }
  frame.is_dirty = frame.is_dirty || is_dirty;

  // WriteThrough 只在最后一个 guard 释放后刷盘，避免复制不稳定内容。
  if (frame.state == FrameState::Valid && flush_policy_ == FlushPolicy::WriteThrough &&
      frame.is_dirty && frame.pin_count == 1) {
    --frame.pin_count;
    frame.state = FrameState::Flushing;
    ++frame.pin_count;
    lock.unlock();
    Status flush_status = Status::Ok();
    {
      std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
      flush_status = FlushFrameToDisk(page_id, frame.page, frame.page_lsn);
    }
    lock.lock();
    if (flush_status.ok()) {
      frame.is_dirty = false;
      frame.page_lsn = kInvalidLsn;
    }
    frame.state = FrameState::Valid;
    --frame.pin_count;
    if (frame.pin_count == 0) (void)replacer_->Unpin(frame_id);
    NotifyStateChange();
    return flush_status;
  }

  --frame.pin_count;
  if (frame.pin_count == 0) {
    const auto unpin_status = frame.state == FrameState::DeletePending
                                  ? Status::Ok()
                                  : replacer_->Unpin(frame_id);
    NotifyStateChange();
    return unpin_status;
  }
  NotifyStateChange();
  return Status::Ok();
}

Result<ReadPageGuard> BufferPoolManager::FetchPage(page_id_t page_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return Result<ReadPageGuard>(ready);
  auto valid = ValidateDataPageId(page_id);
  if (!valid.ok()) return Result<ReadPageGuard>(valid);
  ++access_count_;

  for (;;) {
    if (delete_pending_pages_.find(page_id) != delete_pending_pages_.end()) {
      return Result<ReadPageGuard>(Status::NotFound("页面正在删除: " + std::to_string(page_id)));
    }
    const auto found = page_table_.find(page_id);
    if (found != page_table_.end()) {
      const frame_id_t frame_id = found->second;
      Frame &frame = frames_[frame_id];
      if (frame.state == FrameState::Valid) {
        const auto pin_status = replacer_->Pin(frame_id);
        if (!pin_status.ok()) return Result<ReadPageGuard>(pin_status);
        ++frame.pin_count;
        const auto access_status = replacer_->RecordAccess(frame_id);
        if (!access_status.ok()) {
          --frame.pin_count;
          (void)replacer_->Unpin(frame_id);
          return Result<ReadPageGuard>(access_status);
        }
        ++hit_count_;
        lock.unlock();
        return Result<ReadPageGuard>(ReadPageGuard(this, frame_id, page_id, &frame.latch));
      }
      if (frame.state == FrameState::DeletePending) {
        return Result<ReadPageGuard>(Status::NotFound("页面正在删除: " + std::to_string(page_id)));
      }
      ++loading_waiters_[page_id];
      state_changed_.wait(lock, [&] {
        const auto current = page_table_.find(page_id);
        return current == page_table_.end() || frames_[current->second].state == FrameState::Valid ||
               frames_[current->second].state == FrameState::DeletePending;
      });
      auto failure = load_failures_.find(page_id);
      if (failure != load_failures_.end()) {
        const Status status = failure->second;
        auto waiters = loading_waiters_.find(page_id);
        if (waiters != loading_waiters_.end() && --waiters->second == 0) {
          loading_waiters_.erase(waiters);
          load_failures_.erase(failure);
        }
        return Result<ReadPageGuard>(status);
      }
      auto waiters = loading_waiters_.find(page_id);
      if (waiters != loading_waiters_.end() && waiters->second != 0) --waiters->second;
      continue;
    }

    auto failure = load_failures_.find(page_id);
    if (failure != load_failures_.end()) {
      const Status status = failure->second;
      if (loading_waiters_.find(page_id) == loading_waiters_.end()) load_failures_.erase(failure);
      return Result<ReadPageGuard>(status);
    }

    ++miss_count_;
    auto selected = SelectFrameUnlocked();
    if (!selected.ok()) return Result<ReadPageGuard>(selected.status());
    const FrameSelection selection = selected.value();
    const frame_id_t frame_id = selection.frame_id;
    Frame &frame = frames_[frame_id];
    const page_id_t old_page_id = frame.page_id;
    if (selection.from_free_list) {
      frame.page_id = page_id;
      frame.pin_count = 1;
    } else {
      page_table_[page_id] = frame_id;
    }
    if (selection.from_free_list) page_table_[page_id] = frame_id;
    lock.unlock();

    Result<Page> loaded(Status::IOError("页面读取未完成"));
    Status write_status = Status::Ok();
    {
      std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
      if (!selection.from_free_list && frame.is_dirty) {
        write_status = FlushFrameToDisk(old_page_id, frame.page, frame.page_lsn);
        if (write_status.ok()) frame.page_lsn = kInvalidLsn;
      }
      if (write_status.ok()) loaded = disk_manager_->ReadPage(page_id);
      if (loaded.ok()) {
        frame.page = std::move(loaded.value());
      } else if (selection.from_free_list) {
        frame.page = Page{};
      }
    }

    lock.lock();
    if (!write_status.ok() || !loaded.ok()) {
      const Status status = !write_status.ok() ? write_status : loaded.status();
      page_table_.erase(page_id);
      if (selection.from_free_list) {
        frame.page = Page{};
        frame.page_id = INVALID_PAGE_ID;
        frame.pin_count = 0;
        frame.is_dirty = false;
        frame.state = FrameState::Empty;
        (void)ReturnFreeFrameUnlocked(frame_id);
      } else {
        frame.pin_count = 0;
        frame.state = FrameState::Valid;
        (void)replacer_->Unpin(frame_id);
      }
      load_failures_[page_id] = status;
      NotifyStateChange();
      return Result<ReadPageGuard>(status);
    }

    if (!selection.from_free_list) {
      page_table_.erase(old_page_id);
      if (old_page_id != INVALID_PAGE_ID) {
        ++eviction_count_;
        if (eviction_log_.size() >= 1024) eviction_log_.erase(eviction_log_.begin());
        eviction_log_.push_back(old_page_id);
      }
    }
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    frame.page_lsn = kInvalidLsn;
    frame.state = FrameState::Valid;
    (void)replacer_->Remove(frame_id);
    (void)replacer_->RecordAccess(frame_id);
    NotifyStateChange();
    lock.unlock();
    return Result<ReadPageGuard>(ReadPageGuard(this, frame_id, page_id, &frame.latch));
  }
  }
Result<WritePageGuard> BufferPoolManager::FetchPageWrite(page_id_t page_id) {
  return FetchPageWrite(page_id, nullptr);
}

Result<WritePageGuard> BufferPoolManager::FetchPageWrite(page_id_t page_id,
                                                         Transaction *transaction) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return Result<WritePageGuard>(ready);
  if (transaction != nullptr && !transaction->IsUsableForWork()) {
    return Result<WritePageGuard>(Status::InvalidArgument(
        "FetchPageWrite requires an Active transaction"));
  }
  auto valid = ValidateDataPageId(page_id);
  if (!valid.ok()) return Result<WritePageGuard>(valid);
  ++access_count_;

  for (;;) {
    if (delete_pending_pages_.find(page_id) != delete_pending_pages_.end()) {
      return Result<WritePageGuard>(Status::NotFound("页面正在删除: " + std::to_string(page_id)));
    }
    const auto found = page_table_.find(page_id);
    if (found != page_table_.end()) {
      const frame_id_t frame_id = found->second;
      Frame &frame = frames_[frame_id];
      if (frame.state == FrameState::Valid) {
        const auto pin_status = replacer_->Pin(frame_id);
        if (!pin_status.ok()) return Result<WritePageGuard>(pin_status);
        ++frame.pin_count;
        const auto access_status = replacer_->RecordAccess(frame_id);
        if (!access_status.ok()) {
          --frame.pin_count;
          (void)replacer_->Unpin(frame_id);
          return Result<WritePageGuard>(access_status);
        }
        ++hit_count_;
        lock.unlock();
        return Result<WritePageGuard>(WritePageGuard(this, frame_id, page_id, &frame.latch,
                                                     transaction));
      }
      if (frame.state == FrameState::DeletePending) {
        return Result<WritePageGuard>(Status::NotFound("页面正在删除: " + std::to_string(page_id)));
      }
      ++loading_waiters_[page_id];
      state_changed_.wait(lock, [&] {
        const auto current = page_table_.find(page_id);
        return current == page_table_.end() || frames_[current->second].state == FrameState::Valid ||
               frames_[current->second].state == FrameState::DeletePending;
      });
      auto failure = load_failures_.find(page_id);
      if (failure != load_failures_.end()) {
        const Status status = failure->second;
        auto waiters = loading_waiters_.find(page_id);
        if (waiters != loading_waiters_.end() && --waiters->second == 0) {
          loading_waiters_.erase(waiters);
          load_failures_.erase(failure);
        }
        return Result<WritePageGuard>(status);
      }
      auto waiters = loading_waiters_.find(page_id);
      if (waiters != loading_waiters_.end() && waiters->second != 0) --waiters->second;
      continue;
    }
    auto failure = load_failures_.find(page_id);
    if (failure != load_failures_.end()) {
      const Status status = failure->second;
      if (loading_waiters_.find(page_id) == loading_waiters_.end()) load_failures_.erase(failure);
      return Result<WritePageGuard>(status);
    }

    ++miss_count_;
    auto selected = SelectFrameUnlocked();
    if (!selected.ok()) return Result<WritePageGuard>(selected.status());
    const FrameSelection selection = selected.value();
    const frame_id_t frame_id = selection.frame_id;
    Frame &frame = frames_[frame_id];
    const page_id_t old_page_id = frame.page_id;
    if (selection.from_free_list) {
      frame.page_id = page_id;
      frame.pin_count = 1;
    }
    page_table_[page_id] = frame_id;
    lock.unlock();

    Result<Page> loaded(Status::IOError("页面读取未完成"));
    Status write_status = Status::Ok();
    {
      std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
      if (!selection.from_free_list && frame.is_dirty) {
        write_status = FlushFrameToDisk(old_page_id, frame.page, frame.page_lsn);
        if (write_status.ok()) frame.page_lsn = kInvalidLsn;
      }
      if (write_status.ok()) loaded = disk_manager_->ReadPage(page_id);
      if (loaded.ok()) {
        frame.page = std::move(loaded.value());
      } else if (selection.from_free_list) {
        frame.page = Page{};
      }
    }
    lock.lock();
    if (!write_status.ok() || !loaded.ok()) {
      const Status status = !write_status.ok() ? write_status : loaded.status();
      page_table_.erase(page_id);
      if (selection.from_free_list) {
        frame.page = Page{};
        frame.page_id = INVALID_PAGE_ID;
        frame.pin_count = 0;
        frame.is_dirty = false;
        frame.state = FrameState::Empty;
        (void)ReturnFreeFrameUnlocked(frame_id);
      } else {
        frame.pin_count = 0;
        frame.state = FrameState::Valid;
        (void)replacer_->Unpin(frame_id);
      }
      load_failures_[page_id] = status;
      NotifyStateChange();
      return Result<WritePageGuard>(status);
    }
    if (!selection.from_free_list) {
      page_table_.erase(old_page_id);
      if (old_page_id != INVALID_PAGE_ID) {
        ++eviction_count_;
        if (eviction_log_.size() >= 1024) eviction_log_.erase(eviction_log_.begin());
        eviction_log_.push_back(old_page_id);
      }
    }
    frame.page_id = page_id;
    frame.pin_count = 1;
    frame.is_dirty = false;
    frame.page_lsn = kInvalidLsn;
    frame.state = FrameState::Valid;
    (void)replacer_->Remove(frame_id);
    (void)replacer_->RecordAccess(frame_id);
    NotifyStateChange();
    lock.unlock();
    return Result<WritePageGuard>(WritePageGuard(this, frame_id, page_id, &frame.latch,
                                                 transaction));
  }
}
Result<WritePageGuard> BufferPoolManager::NewPage() {
  return NewPage(nullptr);
}

Result<WritePageGuard> BufferPoolManager::NewPage(Transaction *transaction) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return Result<WritePageGuard>(ready);
  if (transaction != nullptr && !transaction->IsUsableForWork()) {
    return Result<WritePageGuard>(Status::InvalidArgument(
        "NewPage requires an Active transaction"));
  }
  auto selected = SelectFrameUnlocked();
  if (!selected.ok()) return Result<WritePageGuard>(selected.status());
  const FrameSelection selection = selected.value();
  const frame_id_t frame_id = selection.frame_id;
  Frame &frame = frames_[frame_id];
  const page_id_t old_page_id = frame.page_id;
  if (selection.from_free_list) frame.pin_count = 1;
  lock.unlock();

  Status write_status = Status::Ok();
  {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    if (!selection.from_free_list && frame.is_dirty) {
      write_status = FlushFrameToDisk(old_page_id, frame.page, frame.page_lsn);
      if (write_status.ok()) frame.page_lsn = kInvalidLsn;
    }
  }
  Result<page_id_t> allocated(Status::IOError("页面分配未完成"));
  if (write_status.ok()) allocated = disk_manager_->AllocatePage();

  if (!allocated.ok() && selection.from_free_list) {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    frame.page = Page{};
  }

  if (write_status.ok() && allocated.ok() && transaction != nullptr) {
    if (log_manager_ == nullptr) {
      (void)disk_manager_->DeallocatePage(allocated.value());
      allocated = Result<page_id_t>(Status::InvalidArgument(
          "transactional NewPage requires a bound LogManager"));
      transaction->MarkFailed();
    } else {
      LogRecord record;
      record.type = LogRecordType::PageAllocate;
      record.txn_id = transaction->id();
      record.prev_lsn = transaction->last_lsn();
      record.page_id = allocated.value();
      auto append = log_manager_->Append(std::move(record));
      if (!append.ok()) {
        (void)disk_manager_->DeallocatePage(allocated.value());
        allocated = Result<page_id_t>(append.status());
        transaction->MarkFailed();
      } else {
        transaction->SetLastLsn(append.value());
        transaction->AddAllocatedPage(allocated.value());
      }
    }
  }

  lock.lock();
  if (!write_status.ok() || !allocated.ok()) {
    const Status status = !write_status.ok() ? write_status : allocated.status();
    if (selection.from_free_list) {
      frame.page_id = INVALID_PAGE_ID;
      frame.pin_count = 0;
      frame.is_dirty = false;
      frame.state = FrameState::Empty;
      (void)ReturnFreeFrameUnlocked(frame_id);
    } else {
      frame.pin_count = 0;
      frame.state = FrameState::Valid;
      (void)replacer_->Unpin(frame_id);
    }
    NotifyStateChange();
    return Result<WritePageGuard>(status);
  }

  // Keep a selected victim intact until allocation and its WAL record both succeed.
  // Otherwise a failed PageAllocate append could leave the old page id mapped to a
  // zeroed frame.
  {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    frame.page = Page{};
  }

  if (!selection.from_free_list) {
    page_table_.erase(old_page_id);
    ++eviction_count_;
    if (eviction_log_.size() >= 1024) eviction_log_.erase(eviction_log_.begin());
    eviction_log_.push_back(old_page_id);
  }
  frame.page_id = allocated.value();
  frame.pin_count = 1;
  frame.is_dirty = false;
  frame.page_lsn = kInvalidLsn;
  frame.state = FrameState::Valid;
  page_table_[frame.page_id] = frame_id;
  (void)replacer_->Remove(frame_id);
  (void)replacer_->RecordAccess(frame_id);
  NotifyStateChange();
  lock.unlock();
  return Result<WritePageGuard>(WritePageGuard(this, frame_id, frame.page_id, &frame.latch,
                                               transaction));
}
Status BufferPoolManager::UnpinPage(page_id_t page_id, bool is_dirty) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return ready;
  const auto found = page_table_.find(page_id);
  if (found == page_table_.end()) {
    return Status::NotFound("页面不在 BufferPool 中: " + std::to_string(page_id));
  }
  const frame_id_t frame_id = found->second;
  Frame &frame = frames_[frame_id];
  if (frame.pin_count == 0) {
    return Status::InvalidArgument("页面不能重复 Unpin: " + std::to_string(page_id));
  }
  frame.is_dirty = frame.is_dirty || is_dirty;
  if (frame.state == FrameState::Valid && flush_policy_ == FlushPolicy::WriteThrough &&
      frame.is_dirty && frame.pin_count == 1) {
    --frame.pin_count;
    frame.state = FrameState::Flushing;
    ++frame.pin_count;
    lock.unlock();
    Status flush_status = Status::Ok();
    {
      std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
      flush_status = FlushFrameToDisk(page_id, frame.page, frame.page_lsn);
    }
    lock.lock();
    if (flush_status.ok()) {
      frame.is_dirty = false;
      frame.page_lsn = kInvalidLsn;
    }
    frame.state = FrameState::Valid;
    --frame.pin_count;
    if (frame.pin_count == 0) (void)replacer_->Unpin(frame_id);
    NotifyStateChange();
    return flush_status;
  }
  --frame.pin_count;
  if (frame.pin_count == 0) {
    const auto unpin_status = frame.state == FrameState::DeletePending
                                  ? Status::Ok()
                                  : replacer_->Unpin(frame_id);
    NotifyStateChange();
    return unpin_status;
  }
  NotifyStateChange();
  return Status::Ok();
  }
Status BufferPoolManager::FlushPage(page_id_t page_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return ready;
  const auto found = page_table_.find(page_id);
  if (found == page_table_.end()) {
    return Status::NotFound("页面不在 BufferPool 中，无法 Flush: " + std::to_string(page_id));
  }
  const frame_id_t frame_id = found->second;
  Frame &frame = frames_[frame_id];
  if (frame.state == FrameState::DeletePending) {
    return Status::NotFound("页面正在删除，无法 Flush: " + std::to_string(page_id));
  }
  while (frame.state != FrameState::Valid) {
    state_changed_.wait(lock, [&] {
      const auto current = page_table_.find(page_id);
      return current == page_table_.end() || frame.state == FrameState::Valid ||
             frame.state == FrameState::DeletePending;
    });
    if (page_table_.find(page_id) == page_table_.end()) {
      return Status::NotFound("页面加载失败，无法 Flush: " + std::to_string(page_id));
    }
    if (frame.state == FrameState::DeletePending) {
      return Status::NotFound("页面正在删除，无法 Flush: " + std::to_string(page_id));
    }
  }
  const bool was_unpinned = frame.pin_count == 0;
  if (was_unpinned) {
    const auto pin_status = replacer_->Pin(frame_id);
    if (!pin_status.ok()) return pin_status;
  }
  frame.state = FrameState::Flushing;
  ++frame.pin_count;
  if (!frame.is_dirty && frame.pin_count > 1) {
    frame.state = FrameState::Valid;
    --frame.pin_count;
    NotifyStateChange();
    return Status::Ok();
  }
  state_changed_.wait(lock, [&] {
    return frame.state != FrameState::Flushing || frame.pin_count == 1;
  });
  if (frame.state != FrameState::Flushing) {
    return Status::InvalidArgument("页面 Flush 状态已改变: " + std::to_string(page_id));
  }
  lock.unlock();
  Status write_status = Status::Ok();
  {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    if (frame.is_dirty) write_status = FlushFrameToDisk(page_id, frame.page, frame.page_lsn);
  }
  lock.lock();
  if (write_status.ok()) {
    frame.is_dirty = false;
    frame.page_lsn = kInvalidLsn;
  }
  frame.state = FrameState::Valid;
  --frame.pin_count;
  if (frame.pin_count == 0) (void)replacer_->Unpin(frame_id);
  NotifyStateChange();
  return write_status;
  }
Status BufferPoolManager::FlushAllPages() {
  std::vector<page_id_t> pages;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto ready = EnsureReadyUnlocked();
    if (!ready.ok()) return ready;
    pages.reserve(frames_.size());
    for (const auto &frame : frames_) {
      if (frame.page_id != INVALID_PAGE_ID && frame.state != FrameState::Empty) {
        pages.push_back(frame.page_id);
      }
    }
  }
  for (const auto page_id : pages) {
    const auto status = FlushPage(page_id);
    if (!status.ok() && status.code() != ErrorCode::NotFound) return status;
  }
  return Status::Ok();
  }
Status BufferPoolManager::DeletePage(page_id_t page_id) {
  const auto begin = BeginPageDeletion(page_id);
  if (!begin.ok()) return begin;
  return FinalizePageDeletion(page_id);
}

Status BufferPoolManager::DeletePage(page_id_t page_id, Transaction *transaction) {
  if (transaction == nullptr) return DeletePage(page_id);
  if (!transaction->IsUsableForWork()) {
    return Status::InvalidArgument("DeletePage requires an Active transaction");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (delete_pending_pages_.find(page_id) != delete_pending_pages_.end()) {
      // HeapTable may reserve a page while its last guard is still held.
      return AppendPageFreeLog(page_id, transaction);
    }
  }
  auto begin = BeginPageDeletion(page_id);
  if (!begin.ok()) return begin;
  auto log_status = AppendPageFreeLog(page_id, transaction);
  if (!log_status.ok()) {
    (void)CancelPageDeletion(page_id);
    return log_status;
  }
  return Status::Ok();
}

Status BufferPoolManager::BeginPageDeletion(page_id_t page_id) {
  return BeginPageDeletion(page_id, 0);
}

Status BufferPoolManager::BeginPageDeletion(page_id_t page_id, std::uint32_t expected_pins) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return ready;
  auto valid = ValidateDataPageId(page_id);
  if (!valid.ok()) return valid;
  if (!delete_pending_pages_.insert(page_id).second) {
    return Status::AlreadyExists("页面已经处于删除预留状态: " + std::to_string(page_id));
  }
  const auto found = page_table_.find(page_id);
  if (found != page_table_.end()) {
    Frame &frame = frames_[found->second];
    if (frame.state != FrameState::Valid || frame.pin_count != expected_pins) {
      delete_pending_pages_.erase(page_id);
      return Status::InvalidArgument("被 pin 的页面不能删除: " + std::to_string(page_id));
    }
    const auto pin_status = replacer_->Pin(found->second);
    if (!pin_status.ok()) {
      delete_pending_pages_.erase(page_id);
      return pin_status;
    }
    frame.state = FrameState::DeletePending;
  }
  NotifyStateChange();
  return Status::Ok();
}

Status BufferPoolManager::CancelPageDeletion(page_id_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (delete_pending_pages_.find(page_id) == delete_pending_pages_.end()) {
    return Status::NotFound("页面没有删除预留: " + std::to_string(page_id));
  }
  const auto found = page_table_.find(page_id);
  if (found != page_table_.end()) {
    Frame &frame = frames_[found->second];
    if (frame.state != FrameState::DeletePending || frame.pin_count != 0) {
      return Status::InvalidArgument("页面删除预留正在使用: " + std::to_string(page_id));
    }
    frame.state = FrameState::Valid;
    (void)replacer_->Unpin(found->second);
  }
  delete_pending_pages_.erase(page_id);
  NotifyStateChange();
  return Status::Ok();
}

Status BufferPoolManager::FinalizePageDeletion(page_id_t page_id) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto ready = EnsureReadyUnlocked();
  if (!ready.ok()) return ready;
  if (delete_pending_pages_.find(page_id) == delete_pending_pages_.end()) {
    return Status::NotFound("页面没有删除预留: " + std::to_string(page_id));
  }
  const auto found = page_table_.find(page_id);
  if (found == page_table_.end()) {
    lock.unlock();
    auto status = disk_manager_->DeallocatePage(page_id);
    lock.lock();
    if (status.ok() || status.code() == ErrorCode::AlreadyExists) {
      delete_pending_pages_.erase(page_id);
      if (status.code() == ErrorCode::AlreadyExists) status = Status::Ok();
    }
    NotifyStateChange();
    return status;
  }

  const frame_id_t frame_id = found->second;
  Frame &frame = frames_[frame_id];
  if (frame.state != FrameState::DeletePending || frame.pin_count != 0) {
    return Status::InvalidArgument("页面删除预留仍有引用: " + std::to_string(page_id));
  }
  frame.pin_count = 1;
  lock.unlock();

  Status write_status = Status::Ok();
  {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    if (frame.is_dirty) write_status = FlushFrameToDisk(page_id, frame.page, frame.page_lsn);
  }
  Status delete_status = write_status;
  if (delete_status.ok()) delete_status = disk_manager_->DeallocatePage(page_id);
  if (delete_status.code() == ErrorCode::AlreadyExists) delete_status = Status::Ok();
  if (delete_status.ok()) {
    std::unique_lock<std::shared_mutex> frame_lock(frame.latch);
    frame.page = Page{};
  }

  lock.lock();
  if (!delete_status.ok()) {
    frame.pin_count = 0;
    frame.state = FrameState::DeletePending;
    NotifyStateChange();
    return delete_status;
  }
  page_table_.erase(found);
  frame.page_id = INVALID_PAGE_ID;
  frame.pin_count = 0;
  frame.is_dirty = false;
  frame.state = FrameState::Empty;
  delete_pending_pages_.erase(page_id);
  (void)replacer_->Remove(frame_id);
  const auto free_status = ReturnFreeFrameUnlocked(frame_id);
  NotifyStateChange();
  return free_status;
}

Status BufferPoolManager::DeletePages(const std::vector<page_id_t> &page_ids) {
  std::unordered_set<page_id_t> seen;
  for (const auto page_id : page_ids) {
    auto valid = ValidateDataPageId(page_id);
    if (!valid.ok()) return valid;
    if (!seen.insert(page_id).second) {
      return Status::InvalidArgument("批量删除列表包含重复页面: " + std::to_string(page_id));
    }
  }
  std::vector<page_id_t> reserved;
  reserved.reserve(page_ids.size());
  for (const auto page_id : page_ids) {
    const auto status = BeginPageDeletion(page_id);
    if (!status.ok()) {
      for (const auto reserved_id : reserved) (void)CancelPageDeletion(reserved_id);
      return status;
    }
    reserved.push_back(page_id);
  }
  for (std::size_t i = 0; i < reserved.size(); ++i) {
    const auto status = FinalizePageDeletion(reserved[i]);
    if (!status.ok()) {
      for (std::size_t j = i + 1; j < reserved.size(); ++j) {
        (void)CancelPageDeletion(reserved[j]);
      }
      return status;
    }
  }
  return Status::Ok();
}

Status BufferPoolManager::DeletePages(const std::vector<page_id_t> &page_ids,
                                      Transaction *transaction) {
  if (transaction == nullptr) return DeletePages(page_ids);
  std::unordered_set<page_id_t> seen;
  for (const auto page_id : page_ids) {
    if (!seen.insert(page_id).second) {
      return Status::InvalidArgument("DeletePages contains a duplicate page");
    }
  }
  std::vector<page_id_t> reserved;
  for (const auto page_id : page_ids) {
    auto status = DeletePage(page_id, transaction);
    if (!status.ok()) {
      for (const auto reserved_id : reserved) (void)CancelPageDeletion(reserved_id);
      return status;
    }
    reserved.push_back(page_id);
  }
  return Status::Ok();
}
Status BufferPoolManager::Close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return Status::Ok();
  }
  const auto flush_status = FlushAllPages();
  if (!flush_status.ok()) return flush_status;
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) return Status::Ok();
  if (!delete_pending_pages_.empty()) {
    return Status::InvalidArgument("BufferPool 仍有待完成的页面删除");
  }
  closed_ = true;
  NotifyStateChange();
  return Status::Ok();
  }
const Status &BufferPoolManager::GetInitStatus() const noexcept { return init_status_; }

std::uint64_t BufferPoolManager::GetAccessCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return access_count_;
}

std::uint64_t BufferPoolManager::GetHitCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hit_count_;
}

std::uint64_t BufferPoolManager::GetMissCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return miss_count_;
}

std::uint64_t BufferPoolManager::GetEvictionCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return eviction_count_;
}

double BufferPoolManager::GetHitRate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return access_count_ == 0 ? 0.0 : static_cast<double>(hit_count_) / access_count_;
}

std::vector<page_id_t> BufferPoolManager::GetEvictionLog() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return eviction_log_;
}

void BufferPoolManager::ResetStatistics() {
  std::lock_guard<std::mutex> lock(mutex_);
  access_count_ = 0;
  hit_count_ = 0;
  miss_count_ = 0;
  eviction_count_ = 0;
  eviction_log_.clear();
}

std::vector<FrameSnapshot> BufferPoolManager::GetFrameSnapshots() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FrameSnapshot> snapshots;
  snapshots.reserve(frames_.size());
  for (frame_id_t frame_id = 0; frame_id < frames_.size(); ++frame_id) {
    const auto &frame = frames_[frame_id];
    FrameSnapshot snapshot;
    snapshot.frame_id = frame_id;
    if (frame.page_id != INVALID_PAGE_ID) snapshot.page_id = frame.page_id;
    snapshot.pin_count = frame.pin_count;
    snapshot.is_dirty = frame.is_dirty;
    snapshot.state = frame.state;
    snapshot.is_evictable = frame.page_id != INVALID_PAGE_ID &&
                            frame.pin_count == 0 && replacer_->IsEvictable(frame_id);
    snapshots.push_back(snapshot);
  }
  return snapshots;
}

ReplacementPolicy BufferPoolManager::GetReplacementPolicy() const noexcept {
  return replacement_policy_;
}

FlushPolicy BufferPoolManager::GetFlushPolicy() const noexcept { return flush_policy_; }

std::size_t BufferPoolManager::GetPoolSize() const noexcept { return pool_size_; }

}  // namespace oursql
