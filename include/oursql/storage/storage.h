#pragma once

#include "oursql/common/status.h"
#include "oursql/common/types.h"
#include "oursql/transaction/transaction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oursql {

class Catalog;
class LogManager;
class LockManager;

class Page {
 public:
  static constexpr std::size_t kSize = 4096;

  [[nodiscard]] std::byte *Data() noexcept {
    return data_.data();
  }

  [[nodiscard]] const std::byte *Data() const noexcept {
    return data_.data();
  }

 private:
  std::array<std::byte, kSize> data_{};
};

class DiskManager {
 public:
  DiskManager() = default;
  explicit DiskManager(std::filesystem::path file_path);
  ~DiskManager();

  DiskManager(const DiskManager &) = delete;
  DiskManager &operator=(const DiskManager &) = delete;
  DiskManager(DiskManager &&) = delete;
  DiskManager &operator=(DiskManager &&) = delete;

  // 调用者：上层存储或程序入口；作用：打开或创建数据库文件；返回：成功或错误状态。
  [[nodiscard]] Status Open(const std::filesystem::path &file_path);

  // 调用者：上层存储或程序退出流程；作用：关闭数据库文件；返回：成功或错误状态。
  [[nodiscard]] Status Close();

  // 调用者：缓冲池或测试；作用：读取指定页面；返回：页面内容或错误状态。
  [[nodiscard]] Result<Page> ReadPage(page_id_t page_id);

  // 调用者：缓冲池或测试；作用：写入指定页面；返回：成功或错误状态。
  [[nodiscard]] Status WritePage(page_id_t page_id, const Page &page);

  // 调用者：缓冲池或测试；作用：申请一个页面；返回：新页面编号或错误状态。
  [[nodiscard]] Result<page_id_t> AllocatePage();

  // 调用者：缓冲池或测试；作用：释放一个页面；返回：成功或错误状态。
  [[nodiscard]] Status DeallocatePage(page_id_t page_id);

  // 调用者：测试或统计面板；作用：查询逻辑读次数；返回：当前累计值。
  [[nodiscard]] std::uint64_t GetReadCount() const;

  // 调用者：测试或统计面板；作用：查询逻辑写次数；返回：当前累计值。
  [[nodiscard]] std::uint64_t GetWriteCount() const;

  // 调用者：测试或统计面板；作用：清零 I/O 统计；返回：无。
  void ResetIoStatistics();

 protected:
  // 调用者：继承类或内部元数据管理；作用：读取目录头页面编号；返回：当前目录头或 INVALID_PAGE_ID。
  [[nodiscard]] page_id_t GetCatalogHead() const;

  // 调用者：继承类或内部元数据管理；作用：设置目录头页面编号；返回：成功或错误状态。
  [[nodiscard]] Status SetCatalogHead(page_id_t page_id);

 private:
  friend class Catalog;

  static constexpr std::uint32_t kFormatVersion = 1;

  Status CloseUnlocked();
  Status OpenNewFileUnlocked(const std::filesystem::path &file_path);
  Status OpenExistingFileUnlocked(const std::filesystem::path &file_path);
  Status PersistSuperblockUnlocked();
  Result<Page> ReadPhysicalPageUnlocked(page_id_t page_id);
  Status WritePhysicalPageUnlocked(page_id_t page_id, const Page &page);
  Status EnsureFileSizeUnlocked(std::uint64_t page_count);
  Result<page_id_t> PopFreePageUnlocked();
  Status PushFreePageUnlocked(page_id_t page_id);
  Status ValidateMetadataUnlocked();

  std::filesystem::path file_path_;
  std::fstream file_;
  mutable std::mutex mutex_;
  bool is_open_{false};
  page_id_t page_count_{1};
  page_id_t free_list_head_{INVALID_PAGE_ID};
  page_id_t catalog_head_{INVALID_PAGE_ID};
  std::unordered_set<page_id_t> free_pages_;
  std::uint64_t read_count_{0};
  std::uint64_t write_count_{0};
};

using frame_id_t = std::size_t;

enum class ReplacementPolicy {
  FIFO,
  LRU,
  CLOCK,
};

enum class FlushPolicy {
  WriteBack,
  WriteThrough,
};

// BufferPool 内部状态由全局 mutex 保护，页面字节由 Frame latch 保护。
enum class FrameState {
  Empty,
  Loading,
  Valid,
  Flushing,
  Evicting,
  DeletePending,
};

// 调用者：BufferPoolManager；作用：管理可淘汰的未 pin frame；返回：替换操作状态。
class Replacer {
 public:
  virtual ~Replacer() = default;
  // 调用者：缓冲池；作用：将 frame 标记为不可淘汰；返回：操作状态。
  [[nodiscard]] virtual Status Pin(frame_id_t frame_id) = 0;
  // 调用者：缓冲池；作用：将未 pin frame 加入候选集合；返回：操作状态。
  [[nodiscard]] virtual Status Unpin(frame_id_t frame_id) = 0;
  // 调用者：缓冲池；作用：记录页面成功访问或新页面进入缓冲池；返回：操作状态。
  [[nodiscard]] virtual Status RecordAccess(frame_id_t frame_id) = 0;
  // 调用者：缓冲池；作用：清除 frame 的历史和候选状态；返回：操作状态。
  [[nodiscard]] virtual Status Remove(frame_id_t frame_id) = 0;
  // 调用者：缓冲池；作用：选择一个候选 frame；返回：frame 编号或错误。
  [[nodiscard]] virtual Result<frame_id_t> Victim() = 0;
  // 调用者：缓冲池或测试；作用：查询候选 frame 数量；返回：数量。
  [[nodiscard]] virtual std::size_t Size() const = 0;
  [[nodiscard]] virtual bool IsEvictable(frame_id_t frame_id) const = 0;
};

// 调用者：缓冲池或替换策略测试；作用：按进入候选集合顺序淘汰 frame；返回：确定性替换器。
class FIFOReplacer final : public Replacer {
 public:
  explicit FIFOReplacer(std::size_t capacity);
  [[nodiscard]] Status Pin(frame_id_t frame_id) override;
  [[nodiscard]] Status Unpin(frame_id_t frame_id) override;
  [[nodiscard]] Status RecordAccess(frame_id_t frame_id) override;
  [[nodiscard]] Status Remove(frame_id_t frame_id) override;
  [[nodiscard]] Result<frame_id_t> Victim() override;
  [[nodiscard]] std::size_t Size() const override;
  [[nodiscard]] bool IsEvictable(frame_id_t frame_id) const override;

 private:
  std::size_t capacity_{0};
  mutable std::mutex mutex_;
  std::list<frame_id_t> order_;
  std::unordered_set<frame_id_t> candidates_;
  std::unordered_map<frame_id_t, std::uint64_t> arrival_order_;
  std::uint64_t next_arrival_{0};
};

// 调用者：缓冲池或替换策略测试；作用：按最近成为候选的时间淘汰 frame；返回：确定性替换器。
class LRUReplacer final : public Replacer {
 public:
  explicit LRUReplacer(std::size_t capacity);
  [[nodiscard]] Status Pin(frame_id_t frame_id) override;
  [[nodiscard]] Status Unpin(frame_id_t frame_id) override;
  [[nodiscard]] Status RecordAccess(frame_id_t frame_id) override;
  [[nodiscard]] Status Remove(frame_id_t frame_id) override;
  [[nodiscard]] Result<frame_id_t> Victim() override;
  [[nodiscard]] std::size_t Size() const override;
  [[nodiscard]] bool IsEvictable(frame_id_t frame_id) const override;

 private:
  std::size_t capacity_{0};
  mutable std::mutex mutex_;
  std::list<frame_id_t> order_;
  std::unordered_set<frame_id_t> candidates_;
  std::unordered_map<frame_id_t, std::uint64_t> last_used_;
  std::uint64_t access_clock_{0};
};

class ClockReplacer final : public Replacer {
 public:
  explicit ClockReplacer(std::size_t capacity);
  [[nodiscard]] Status Pin(frame_id_t frame_id) override;
  [[nodiscard]] Status Unpin(frame_id_t frame_id) override;
  [[nodiscard]] Status RecordAccess(frame_id_t frame_id) override;
  [[nodiscard]] Status Remove(frame_id_t frame_id) override;
  [[nodiscard]] Result<frame_id_t> Victim() override;
  [[nodiscard]] std::size_t Size() const override;
  [[nodiscard]] bool IsEvictable(frame_id_t frame_id) const override;

 private:
  std::size_t capacity_{0};
  mutable std::mutex mutex_;
  std::vector<bool> active_;
  std::vector<bool> evictable_;
  std::vector<bool> reference_bits_;
  std::size_t hand_{0};
  std::size_t evictable_count_{0};
};

class BufferPoolManager;

class ReadPageGuard {
 public:
  ReadPageGuard() = default;
  ~ReadPageGuard();
  ReadPageGuard(const ReadPageGuard &) = delete;
  ReadPageGuard &operator=(const ReadPageGuard &) = delete;
  ReadPageGuard(ReadPageGuard &&other) noexcept;
  ReadPageGuard &operator=(ReadPageGuard &&other) noexcept;

  // 调用者：读查询；作用：访问 guard 持有页面的只读字节；返回：页面数据指针，生命周期受 guard 保护。
  [[nodiscard]] const std::byte *Data() const noexcept;
  // 调用者：读查询；作用：查询当前页面编号；返回：页面编号或 INVALID_PAGE_ID。
  [[nodiscard]] page_id_t PageId() const noexcept;
  // 调用者：上层存储；作用：查询 guard 是否持有页面；返回：有效性。
  [[nodiscard]] bool IsValid() const noexcept;

 private:
  friend class BufferPoolManager;
  ReadPageGuard(BufferPoolManager *buffer_pool, frame_id_t frame_id, page_id_t page_id,
                std::shared_mutex *latch);
  void Reset() noexcept;

  BufferPoolManager *buffer_pool_{nullptr};
  frame_id_t frame_id_{0};
  page_id_t page_id_{INVALID_PAGE_ID};
  std::shared_lock<std::shared_mutex> latch_;
};

class WritePageGuard {
 public:
  WritePageGuard() = default;
  ~WritePageGuard();
  WritePageGuard(const WritePageGuard &) = delete;
  WritePageGuard &operator=(const WritePageGuard &) = delete;
  WritePageGuard(WritePageGuard &&other) noexcept;
  WritePageGuard &operator=(WritePageGuard &&other) noexcept;

  // 调用者：写入查询或上层存储；作用：访问 guard 持有页面的可写字节；返回：页面数据指针，生命周期受 guard 保护。
  [[nodiscard]] std::byte *Data() noexcept;
  // 调用者：页面元数据读取；作用：通过写 guard 只读访问页面字节；返回：只读页面数据指针。
  [[nodiscard]] const std::byte *Data() const noexcept;
  // 调用者：写入查询；作用：查询当前页面编号；返回：页面编号或 INVALID_PAGE_ID。
  [[nodiscard]] page_id_t PageId() const noexcept;
  // 调用者：修改页面的调用者；作用：显式标记页面已修改；返回：标脏状态。
  [[nodiscard]] Status MarkDirty() noexcept;
  [[nodiscard]] Status Release() noexcept;
  // 调用者：上层存储；作用：查询 guard 是否持有页面；返回：有效性。
  [[nodiscard]] bool IsValid() const noexcept;

 private:
  friend class BufferPoolManager;
  WritePageGuard(BufferPoolManager *buffer_pool, frame_id_t frame_id, page_id_t page_id,
                 std::shared_mutex *latch, Transaction *transaction = nullptr);
  [[nodiscard]] Status Reset() noexcept;

  BufferPoolManager *buffer_pool_{nullptr};
  frame_id_t frame_id_{0};
  page_id_t page_id_{INVALID_PAGE_ID};
  bool dirty_{false};
  Transaction *transaction_{nullptr};
  std::array<std::byte, Page::kSize> before_image_{};
  bool has_before_image_{false};
  std::unique_lock<std::shared_mutex> latch_;
};

class SlottedPage {
 public:
  static constexpr std::uint32_t kPageType = 1;
  static constexpr std::size_t kHeaderSize = 32;
  static constexpr std::size_t kSlotSize = 12;
  static constexpr std::size_t kMaxRecordSize = Page::kSize - kHeaderSize - kSlotSize;
  static constexpr std::size_t kPageTypeOffset = 0;
  static constexpr std::size_t kSlotCountOffset = 4;
  static constexpr std::size_t kFreeStartOffset = 8;
  static constexpr std::size_t kFreeEndOffset = 12;
  static constexpr std::size_t kNextPageIdOffset = 16;
  static constexpr std::size_t kReservedOffset = 20;
  static constexpr std::uint32_t kDeletedSlot = 0;
  static constexpr std::uint32_t kUsedSlot = 1;

  // 调用者：页面创建流程；作用：初始化 Slotted Page 头和空槽目录；返回：操作状态。
  [[nodiscard]] static Status Initialize(WritePageGuard &page,
                                          page_id_t next_page_id = INVALID_PAGE_ID);
  // 调用者：页面读取流程；作用：校验页面头、槽目录和记录边界；返回：校验状态。
  [[nodiscard]] static Status Validate(const ReadPageGuard &page);
  // 调用者：插入流程；作用：插入一条变长原始记录；返回：slot_id 或空间/格式错误。
  [[nodiscard]] static Result<slot_id_t> InsertRecord(WritePageGuard &page,
                                                       const std::vector<std::byte> &record);
  // 调用者：扫描或读取流程；作用：读取有效槽中的原始记录；返回：记录字节或错误。
  [[nodiscard]] static Result<std::vector<std::byte>> GetRecord(const ReadPageGuard &page,
                                                                  slot_id_t slot_id);
  [[nodiscard]] static Result<std::vector<std::byte>> GetRecord(const WritePageGuard &page,
                                                                  slot_id_t slot_id);
  // 调用者：删除流程；作用：将槽标记为 deleted 并释放其逻辑空间；返回：操作状态。
  [[nodiscard]] static Status DeleteRecord(WritePageGuard &page, slot_id_t slot_id);
  // 调用者：页面维护流程；作用：整理有效记录并保持 slot_id 不变；返回：操作状态。
  [[nodiscard]] static Status Compact(WritePageGuard &page);
  // 调用者：页面元数据读取流程；作用：读取槽数量；返回：数量或格式错误。
  [[nodiscard]] static Result<std::uint32_t> SlotCount(const ReadPageGuard &page);
  [[nodiscard]] static Result<std::uint32_t> SlotCount(const WritePageGuard &page);
  // 调用者：页面链表读取流程；作用：读取下一页编号；返回：编号或格式错误。
  [[nodiscard]] static Result<page_id_t> NextPageId(const ReadPageGuard &page);
  // 调用者：页面链表修改流程；作用：从写 guard 读取下一页编号；返回：编号或格式错误。
  [[nodiscard]] static Result<page_id_t> NextPageId(const WritePageGuard &page);
  // 调用者：页面链表修改流程；作用：设置下一页编号；返回：操作状态。
  [[nodiscard]] static Status SetNextPageId(WritePageGuard &page, page_id_t next_page_id);

 private:
  [[nodiscard]] static Status ValidateBytes(const std::byte *data);
};

struct FrameSnapshot {
  frame_id_t frame_id{0};
  std::optional<page_id_t> page_id;
  std::uint32_t pin_count{0};
  bool is_dirty{false};
  bool is_evictable{false};
  FrameState state{FrameState::Empty};
};

class BufferPoolManager {
 public:
  // 调用者：HeapTable 回收空数据页；作用：原子预留未被引用的页面，禁止新的 Fetch；返回：预留状态。
  [[nodiscard]] Status BeginPageDeletion(page_id_t page_id);
  // 调用者：持有目标页唯一 WriteGuard 的 HeapTable；作用：把该 guard 作为唯一允许的预留引用；返回：预留状态。
  [[nodiscard]] Status BeginPageDeletion(page_id_t page_id, std::uint32_t expected_pins);
  // 调用者：HeapTable 摘链失败路径；作用：取消删除预留并恢复页面；返回：恢复状态。
  [[nodiscard]] Status CancelPageDeletion(page_id_t page_id);
  // 调用者：HeapTable 摘链成功路径；作用：完成删除预留并释放磁盘页；返回：释放状态。
  [[nodiscard]] Status FinalizePageDeletion(page_id_t page_id);
  // 调用者：上层数据库；作用：创建缓冲池管理器；返回：持有容量和磁盘句柄的对象。
  explicit BufferPoolManager(std::size_t pool_size, DiskManager *disk_manager,
                             ReplacementPolicy replacement_policy = ReplacementPolicy::FIFO,
                             FlushPolicy flush_policy = FlushPolicy::WriteBack);
  ~BufferPoolManager();

  BufferPoolManager(const BufferPoolManager &) = delete;
  BufferPoolManager &operator=(const BufferPoolManager &) = delete;
  BufferPoolManager(BufferPoolManager &&) noexcept = delete;
  BufferPoolManager &operator=(BufferPoolManager &&) noexcept = delete;

  // 调用者：读查询；作用：获取页面的共享读 guard；返回：guard 或错误状态。
  [[nodiscard]] Result<ReadPageGuard> FetchPage(page_id_t page_id);
  [[nodiscard]] Result<ReadPageGuard> FetchPage(page_id_t page_id,
                                                  Transaction *transaction);
  // 调用者：写查询或存储层；作用：获取页面的独占写 guard；返回：guard 或错误状态。
  [[nodiscard]] Result<WritePageGuard> FetchPageWrite(page_id_t page_id);
  // 调用者：插入或页面创建流程；作用：申请并获取新的空白页面；返回：写 guard 或错误状态。
  [[nodiscard]] Result<WritePageGuard> NewPage();
  // 调用者：HeapTable 或页面创建流程；作用：申请并获取新的写 guard 页面；返回：guard 或错误状态。
  [[nodiscard]] Result<WritePageGuard> NewPageGuarded() { return NewPage(); }
  [[nodiscard]] Status SetLogManager(LogManager *log_manager);
  [[nodiscard]] Status SetLockManager(LockManager *lock_manager);
  [[nodiscard]] Status RestorePage(page_id_t page_id, const Page &page,
                                    bool write_disk = true);
  [[nodiscard]] Result<WritePageGuard> FetchPageWrite(page_id_t page_id,
                                                       Transaction *transaction);
  [[nodiscard]] Result<WritePageGuard> NewPage(Transaction *transaction);
  // 调用者：不使用 guard 的存储代码；作用：释放一次页面 pin 并更新 dirty；返回：操作状态。
  [[nodiscard]] Status UnpinPage(page_id_t page_id, bool is_dirty);
  // 调用者：刷盘或测试；作用：写回一个脏页；返回：操作状态。
  [[nodiscard]] Status FlushPage(page_id_t page_id);
  // 调用者：关闭流程；作用：写回全部脏页；返回：操作状态。
  [[nodiscard]] Status FlushAllPages();
  // 调用者：删除流程；作用：释放未 pin 页面并从缓冲池移除；返回：操作状态。
  [[nodiscard]] Status DeletePage(page_id_t page_id);
  [[nodiscard]] Status DeletePage(page_id_t page_id, Transaction *transaction);
  // 调用者：表或索引释放流程；作用：预检查后批量释放页面；返回：成功或错误状态。
  [[nodiscard]] Status DeletePages(const std::vector<page_id_t> &page_ids);
  [[nodiscard]] Status DeletePages(const std::vector<page_id_t> &page_ids,
                                   Transaction *transaction);
  // 调用者：关闭流程；作用：刷新并关闭缓冲池；返回：操作状态。
  [[nodiscard]] Status Close();
  // 调用者：启动流程；作用：查询构造期间的配置状态；返回：成功或明确错误。
  [[nodiscard]] const Status &GetInitStatus() const noexcept;
  // 调用者：监控或测试；作用：查询 Fetch 请求次数；返回：累计次数。
  [[nodiscard]] std::uint64_t GetAccessCount() const;
  // 调用者：监控或测试；作用：查询命中次数；返回：累计次数。
  [[nodiscard]] std::uint64_t GetHitCount() const;
  // 调用者：监控或测试；作用：查询未命中次数；返回：累计次数。
  [[nodiscard]] std::uint64_t GetMissCount() const;
  // 调用者：监控或测试；作用：查询淘汰次数；返回：累计次数。
  [[nodiscard]] std::uint64_t GetEvictionCount() const;
  // 调用者：监控或测试；作用：查询命中率；返回：0 到 1 之间的比例。
  [[nodiscard]] double GetHitRate() const;
  // 调用者：监控或测试；作用：取得最近淘汰的页面编号；返回：按发生顺序的日志副本。
  [[nodiscard]] std::vector<page_id_t> GetEvictionLog() const;
  // 调用者：测试或监控；作用：清零缓冲池统计；返回：无。
  void ResetStatistics();
  [[nodiscard]] std::vector<FrameSnapshot> GetFrameSnapshots() const;
  [[nodiscard]] ReplacementPolicy GetReplacementPolicy() const noexcept;
  [[nodiscard]] FlushPolicy GetFlushPolicy() const noexcept;
  [[nodiscard]] std::size_t GetPoolSize() const noexcept;

 private:
  friend class ReadPageGuard;
  friend class WritePageGuard;

  struct Frame {
    // mutex_ 保护下修改元数据；latch 只保护 page 字节，磁盘 I/O 不持有 mutex_。
    Page page;
    page_id_t page_id{INVALID_PAGE_ID};
    std::uint32_t pin_count{0};
    bool is_dirty{false};
    lsn_t page_lsn{kInvalidLsn};
    FrameState state{FrameState::Empty};
    mutable std::shared_mutex latch;
  };

  struct FrameSelection {
    frame_id_t frame_id{0};
    bool from_free_list{false};
  };

  [[nodiscard]] Result<FrameSelection> SelectFrameUnlocked();
  void RestoreSelectionUnlocked(const FrameSelection &selection);
  [[nodiscard]] Status ReturnFreeFrameUnlocked(frame_id_t frame_id);
  [[nodiscard]] Status ReleaseGuard(frame_id_t frame_id, page_id_t page_id, bool is_dirty) noexcept;
  [[nodiscard]] Status RecordGuardUpdate(frame_id_t frame_id, page_id_t page_id,
                                         Transaction *transaction, bool dirty,
                                         const std::array<std::byte, Page::kSize> &before,
                                         bool *logged) noexcept;
  [[nodiscard]] Status AppendPageFreeLog(page_id_t page_id, Transaction *transaction);
  [[nodiscard]] Status AcquirePageLock(page_id_t page_id, Transaction *transaction,
                                       LockMode mode);
  [[nodiscard]] Status FlushFrameToDisk(page_id_t page_id, const Page &page,
                                        lsn_t page_lsn);
  [[nodiscard]] Status EnsureReadyUnlocked() const;
  [[nodiscard]] Status ValidateDataPageId(page_id_t page_id) const;
  void NotifyStateChange() noexcept;

  std::size_t pool_size_{0};
  DiskManager *disk_manager_{nullptr};
  LogManager *log_manager_{nullptr};
  LockManager *lock_manager_{nullptr};
  bool log_manager_bound_{false};
  bool lock_manager_bound_{false};
  ReplacementPolicy replacement_policy_{ReplacementPolicy::FIFO};
  FlushPolicy flush_policy_{FlushPolicy::WriteBack};
  Status init_status_;
  mutable std::mutex mutex_;
  std::condition_variable state_changed_;
  std::vector<Frame> frames_;
  std::deque<frame_id_t> free_frames_;
  std::unordered_set<frame_id_t> free_frame_set_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::unordered_map<page_id_t, Status> load_failures_;
  std::unordered_map<page_id_t, std::size_t> loading_waiters_;
  std::unordered_set<page_id_t> delete_pending_pages_;
  std::unique_ptr<Replacer> replacer_;
  std::uint64_t access_count_{0};
  std::uint64_t hit_count_{0};
  std::uint64_t miss_count_{0};
  std::uint64_t eviction_count_{0};
  std::vector<page_id_t> eviction_log_;
  bool closed_{false};
};

}  // namespace oursql
