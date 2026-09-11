#include "oursql/common/types.h"
#include "oursql/storage/storage.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

class DiskManagerProbe : public oursql::DiskManager {
 public:
  using oursql::DiskManager::GetCatalogHead;
  using oursql::DiskManager::SetCatalogHead;
};

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / ("oursql_disk_manager_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

bool Check(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "[FAIL] " << message << '\n';
  }
  return condition;
}

oursql::Page MakePatternPage(std::uint8_t seed) {
  oursql::Page page;
  for (std::size_t i = 0; i < oursql::Page::kSize; ++i) {
    page.Data()[i] = std::byte{static_cast<unsigned char>((seed + i) & 0xFF)};
  }
  return page;
}

bool PageEquals(const oursql::Page &lhs, const oursql::Page &rhs) {
  for (std::size_t i = 0; i < oursql::Page::kSize; ++i) {
    if (lhs.Data()[i] != rhs.Data()[i]) {
      return false;
    }
  }
  return true;
}

bool OpenDb(oursql::DiskManager &dm, const std::filesystem::path &path) {
  return Check(dm.Open(path).ok(), "打开数据库文件应成功");
}

bool CloseDb(oursql::DiskManager &dm) {
  return Check(dm.Close().ok(), "关闭数据库文件应成功");
}

bool TestNewAllocationSequence() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }

  auto first = dm.AllocatePage();
  if (!Check(first.ok(), "首个新页面申请应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(first.value() == 1, "首个新页面编号应为 1")) {
    CloseDb(dm);
    return false;
  }

  auto second = dm.AllocatePage();
  if (!Check(second.ok(), "第二个新页面申请应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(second.value() == 2, "第二个新页面编号应为 2")) {
    CloseDb(dm);
    return false;
  }

  return CloseDb(dm);
}

bool TestPersistReadWriteAndIsolation() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }

  auto p1 = dm.AllocatePage();
  auto p2 = dm.AllocatePage();
  if (!Check(p1.ok() && p2.ok(), "两页申请应成功")) {
    CloseDb(dm);
    return false;
  }

  const auto page1 = MakePatternPage(0x11);
  const auto page2 = MakePatternPage(0xA7);
  if (!Check(dm.WritePage(p1.value(), page1).ok(), "第一页写入应成功") ||
      !Check(dm.WritePage(p2.value(), page2).ok(), "第二页写入应成功")) {
    CloseDb(dm);
    return false;
  }

  auto read1 = dm.ReadPage(p1.value());
  auto read2 = dm.ReadPage(p2.value());
  if (!Check(read1.ok() && read2.ok(), "写后读取应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(PageEquals(page1, read1.value()), "第一页内容应原样保留")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(PageEquals(page2, read2.value()), "第二页内容应原样保留")) {
    CloseDb(dm);
    return false;
  }

  if (!CloseDb(dm)) {
    return false;
  }

  oursql::DiskManager reopened;
  if (!OpenDb(reopened, temp.path)) {
    return false;
  }
  auto reread1 = reopened.ReadPage(p1.value());
  auto reread2 = reopened.ReadPage(p2.value());
  const bool ok = Check(reread1.ok() && reread2.ok(), "重启后读取应成功") &&
                  Check(PageEquals(page1, reread1.value()), "重启后第一页内容应一致") &&
                  Check(PageEquals(page2, reread2.value()), "重启后第二页内容应一致");
  return CloseDb(reopened) && ok;
}

bool TestReuseFreedPageAfterRestart() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }

  auto first = dm.AllocatePage();
  if (!Check(first.ok(), "申请页面应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.DeallocatePage(first.value()).ok(), "释放页面应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!CloseDb(dm)) {
    return false;
  }

  oursql::DiskManager reopened;
  if (!OpenDb(reopened, temp.path)) {
    return false;
  }
  auto reused = reopened.AllocatePage();
  const bool ok = Check(reused.ok(), "重启后复用空闲页应成功") &&
                  Check(reused.value() == first.value(), "复用的页面编号应与释放的页面一致");
  return CloseDb(reopened) && ok;
}

bool TestErrorHandlingAndStatistics() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }

  dm.ResetIoStatistics();

  auto missing = dm.ReadPage(1);
  if (!Check(!missing.ok(), "越界读取应返回错误")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(missing.status().code() == oursql::ErrorCode::NotFound, "越界读取应返回 NotFound")) {
    CloseDb(dm);
    return false;
  }

  auto invalid = dm.DeallocatePage(oursql::INVALID_PAGE_ID);
  if (!Check(!invalid.ok(), "非法页号释放应返回错误")) {
    CloseDb(dm);
    return false;
  }

  auto page = dm.AllocatePage();
  if (!Check(page.ok(), "申请页面应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.DeallocatePage(page.value()).ok(), "首次释放应成功")) {
    CloseDb(dm);
    return false;
  }
  auto duplicate = dm.DeallocatePage(page.value());
  if (!Check(!duplicate.ok(), "重复释放应返回错误")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(duplicate.code() == oursql::ErrorCode::AlreadyExists, "重复释放应返回 AlreadyExists")) {
    CloseDb(dm);
    return false;
  }

  const auto write_page = dm.AllocatePage();
  if (!Check(write_page.ok(), "再次申请页面应成功")) {
    CloseDb(dm);
    return false;
  }

  dm.ResetIoStatistics();

  const auto payload = MakePatternPage(0x33);
  if (!Check(dm.WritePage(write_page.value(), payload).ok(), "写页面应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.GetWriteCount() == 1, "写统计应为 1")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.GetReadCount() == 0, "读统计应为 0")) {
    CloseDb(dm);
    return false;
  }

  auto reread = dm.ReadPage(write_page.value());
  if (!Check(reread.ok(), "读页面应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(PageEquals(payload, reread.value()), "读回内容应一致")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.GetReadCount() == 1, "读统计应为 1")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.GetWriteCount() == 1, "写统计仍应为 1")) {
    CloseDb(dm);
    return false;
  }

  return CloseDb(dm);
}

bool TestCatalogHeadPersistence() {
  TempDb temp;
  DiskManagerProbe dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }

  auto page = dm.AllocatePage();
  if (!Check(page.ok(), "申请目录头页应成功")) {
    CloseDb(dm);
    return false;
  }

  if (!Check(dm.SetCatalogHead(page.value()).ok(), "设置目录头应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(dm.GetCatalogHead() == page.value(), "目录头应能立即读取")) {
    CloseDb(dm);
    return false;
  }

  if (!CloseDb(dm)) {
    return false;
  }

  DiskManagerProbe reopened;
  if (!OpenDb(reopened, temp.path)) {
    return false;
  }
  const bool ok = Check(reopened.GetCatalogHead() == page.value(), "重启后目录头应保留");
  return CloseDb(reopened) && ok;
}

bool AllocatePages(oursql::DiskManager &dm, std::size_t count,
                   std::vector<oursql::page_id_t> *pages) {
  pages->clear();
  for (std::size_t i = 0; i < count; ++i) {
    auto page = dm.AllocatePage();
    if (!Check(page.ok(), "测试页面申请应成功")) {
      return false;
    }
    pages->push_back(page.value());
  }
  return true;
}

bool TestReplacerPolicies() {
  oursql::FIFOReplacer fifo(3);
  if (!Check(fifo.Unpin(0).ok() && fifo.Unpin(1).ok() && fifo.Unpin(2).ok(),
             "FIFO 候选加入应成功")) {
    return false;
  }
  auto fifo_first = fifo.Victim();
  if (!Check(fifo_first.ok() && fifo_first.value() == 0, "FIFO 应淘汰最早候选")) {
    return false;
  }
  if (!Check(fifo.Pin(1).ok() && fifo.Unpin(1).ok(), "FIFO 重新进入候选应成功")) {
    return false;
  }
  auto fifo_second = fifo.Victim();
  if (!Check(fifo_second.ok() && fifo_second.value() == 1, "FIFO 应恢复页面原到达顺序")) {
    return false;
  }

  oursql::LRUReplacer lru(3);
  if (!Check(lru.Unpin(0).ok() && lru.Unpin(1).ok() && lru.Unpin(2).ok(),
             "LRU 候选加入应成功")) {
    return false;
  }
  if (!Check(lru.Pin(0).ok() && lru.Unpin(0).ok(), "LRU 最近访问更新应成功")) {
    return false;
  }
  auto lru_first = lru.Victim();
  return Check(lru_first.ok() && lru_first.value() == 1, "LRU 应淘汰最久未使用候选");
}

bool TestBufferPoolHitAndPinnedFailure() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }
  std::vector<oursql::page_id_t> pages;
  if (!AllocatePages(dm, 4, &pages)) {
    CloseDb(dm);
    return false;
  }
  dm.ResetIoStatistics();
  oursql::BufferPoolManager bpm(3, &dm);
  {
    auto result = bpm.FetchPage(pages[0]);
    if (!Check(result.ok(), "首次 Fetch 应成功")) {
      CloseDb(dm);
      return false;
    }
    auto guard = std::move(result.value());
    if (!Check(guard.IsValid() && guard.PageId() == pages[0] && guard.Data() != nullptr,
               "读 guard 应有效")) {
      CloseDb(dm);
      return false;
    }
  }
  const auto reads_after_first = dm.GetReadCount();
  {
    auto result = bpm.FetchPage(pages[0]);
    if (!Check(result.ok(), "第二次 Fetch 应成功")) {
      CloseDb(dm);
      return false;
    }
    auto guard = std::move(result.value());
  }
  if (!Check(dm.GetReadCount() == reads_after_first, "命中不应增加磁盘读取")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(bpm.GetAccessCount() == 2 && bpm.GetHitCount() == 1 && bpm.GetMissCount() == 1 &&
                 bpm.GetHitRate() == 0.5,
             "Fetch 统计应与命中操作一致")) {
    CloseDb(dm);
    return false;
  }

  auto pinned_zero = bpm.FetchPage(pages[0]);
  auto first = bpm.FetchPage(pages[1]);
  auto second = bpm.FetchPage(pages[2]);
  if (!Check(pinned_zero.ok() && first.ok() && second.ok(), "填满剩余 frame 应成功")) {
    CloseDb(dm);
    return false;
  }
  auto pinned_zero_guard = std::move(pinned_zero.value());
  auto first_guard = std::move(first.value());
  auto second_guard = std::move(second.value());
  auto fourth = bpm.FetchPage(pages[3]);
  const bool failed = Check(!fourth.ok(), "全部 frame 被 pin 时第 4 页应失败") &&
                      Check(fourth.status().code() == oursql::ErrorCode::NotFound,
                            "无可淘汰 frame 应返回 NotFound");
  return CloseDb(dm) && failed;
}

bool TestFifoAndLruEvictionOrder() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }
  std::vector<oursql::page_id_t> pages;
  if (!AllocatePages(dm, 4, &pages)) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager fifo_bpm(3, &dm, oursql::ReplacementPolicy::FIFO);
  for (std::size_t i = 0; i < 3; ++i) {
    auto result = fifo_bpm.FetchPage(pages[i]);
    if (!Check(result.ok(), "FIFO 预热 Fetch 应成功")) {
      CloseDb(dm);
      return false;
    }
    auto guard = std::move(result.value());
  }
  auto fifo_fetch = fifo_bpm.FetchPage(pages[3]);
  if (!Check(fifo_fetch.ok(), "FIFO 第 4 页 Fetch 应成功")) {
    CloseDb(dm);
    return false;
  }
  auto fifo_guard = std::move(fifo_fetch.value());
  const auto fifo_log = fifo_bpm.GetEvictionLog();
  if (!Check(fifo_log.size() == 1 && fifo_log[0] == pages[0], "FIFO 淘汰日志应为第一个候选页")) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager lru_bpm(3, &dm, oursql::ReplacementPolicy::LRU);
  for (std::size_t i = 0; i < 3; ++i) {
    auto result = lru_bpm.FetchPage(pages[i]);
    if (!Check(result.ok(), "LRU 预热 Fetch 应成功")) {
      CloseDb(dm);
      return false;
    }
    auto guard = std::move(result.value());
  }
  auto recent = lru_bpm.FetchPage(pages[0]);
  if (!Check(recent.ok(), "LRU 最近访问 Fetch 应成功")) {
    CloseDb(dm);
    return false;
  }
  auto recent_guard = std::move(recent.value());
  auto lru_fetch = lru_bpm.FetchPage(pages[3]);
  if (!Check(lru_fetch.ok(), "LRU 第 4 页 Fetch 应成功")) {
    CloseDb(dm);
    return false;
  }
  auto lru_guard = std::move(lru_fetch.value());
  const auto lru_log = lru_bpm.GetEvictionLog();
  const bool ok = Check(lru_log.size() == 1 && lru_log[0] == pages[1],
                        "LRU 应淘汰最近访问之外的最旧页");
  return CloseDb(dm) && ok;
}

bool TestFreeFrameListAndPolicyContrast() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) return false;
  std::vector<oursql::page_id_t> pages;
  if (!AllocatePages(dm, 4, &pages)) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager free_bpm(2, &dm);
  auto fetch_and_release = [](oursql::BufferPoolManager &bpm, oursql::page_id_t page_id) {
    auto result = bpm.FetchPage(page_id);
    if (!result.ok()) return false;
    auto guard = std::move(result.value());
    return guard.IsValid();
  };
  if (!Check(fetch_and_release(free_bpm, pages[0]) && fetch_and_release(free_bpm, pages[1]),
             "容量为 2 时应先使用两个空 frame")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(free_bpm.GetEvictionCount() == 0, "使用空 frame 不应计为 eviction")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(free_bpm.DeletePage(pages[1]).ok(), "删除未 pin 页面应成功归还 frame")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(fetch_and_release(free_bpm, pages[2]) && free_bpm.GetEvictionCount() == 0,
             "删除后应复用空 frame 且不增加 eviction")) {
    CloseDb(dm);
    return false;
  }
  {
    auto retained = free_bpm.FetchPage(pages[0]);
    if (!Check(retained.ok(), "未被删除的缓存页应继续命中")) {
      CloseDb(dm);
      return false;
    }
    auto retained_guard = std::move(retained.value());
  }
  if (!Check(free_bpm.Close().ok(), "FreeList 测试缓冲池关闭应成功")) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager free_failure_bpm(2, &dm);
  auto failed_free_fetch = free_failure_bpm.FetchPage(99);
  if (!Check(!failed_free_fetch.ok(), "空 frame 读取不存在页面应失败")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(fetch_and_release(free_failure_bpm, pages[3]) &&
                 free_failure_bpm.GetEvictionCount() == 0,
             "空 frame 读取失败后应归还并可再次使用")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(free_failure_bpm.Close().ok(), "空 frame 失败回滚测试关闭应成功")) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager failed_bpm(1, &dm);
  if (!Check(fetch_and_release(failed_bpm, pages[0]), "失败回滚测试预热应成功")) {
    CloseDb(dm);
    return false;
  }
  auto missing = failed_bpm.FetchPage(99);
  if (!Check(!missing.ok(), "不存在页面读取应失败")) {
    CloseDb(dm);
    return false;
  }
  {
    auto recovered = failed_bpm.FetchPage(pages[0]);
    if (!Check(recovered.ok(), "牺牲页读取失败后 frame 应恢复为可用候选")) {
      CloseDb(dm);
      return false;
    }
    auto recovered_guard = std::move(recovered.value());
  }
  if (!Check(failed_bpm.Close().ok(), "失败回滚测试缓冲池关闭应成功")) {
    CloseDb(dm);
    return false;
  }

  oursql::BufferPoolManager fifo_bpm(3, &dm, oursql::ReplacementPolicy::FIFO);
  oursql::BufferPoolManager lru_bpm(3, &dm, oursql::ReplacementPolicy::LRU);
  for (const auto page_id : {pages[0], pages[1], pages[2]}) {
    if (!Check(fetch_and_release(fifo_bpm, page_id) && fetch_and_release(lru_bpm, page_id),
               "对照序列预热应成功")) {
      CloseDb(dm);
      return false;
    }
  }
  if (!Check(fetch_and_release(fifo_bpm, pages[0]) && fetch_and_release(lru_bpm, pages[0]),
             "对照序列第二次访问 Page 1 应成功")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(fetch_and_release(fifo_bpm, pages[3]) && fetch_and_release(lru_bpm, pages[3]),
             "对照序列访问 Page 4 应成功")) {
    CloseDb(dm);
    return false;
  }
  const auto fifo_log = fifo_bpm.GetEvictionLog();
  const auto lru_log = lru_bpm.GetEvictionLog();
  if (!Check(fifo_log.size() == 1 && fifo_log[0] == pages[0], "FIFO 对照序列应淘汰 Page 1")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(lru_log.size() == 1 && lru_log[0] == pages[1], "LRU 对照序列应淘汰 Page 2")) {
    CloseDb(dm);
    return false;
  }
  auto fifo_last = fifo_bpm.FetchPage(pages[0]);
  auto lru_last = lru_bpm.FetchPage(pages[0]);
  const bool stats_ok = Check(fifo_last.ok() && lru_last.ok(), "对照序列最后访问应成功") &&
                        Check(fifo_bpm.GetHitCount() == 1 && fifo_bpm.GetMissCount() == 5,
                              "FIFO 对照序列命中未命中统计应为 1/5") &&
                        Check(lru_bpm.GetHitCount() == 2 && lru_bpm.GetMissCount() == 4,
                              "LRU 对照序列命中未命中统计应为 2/4");
  if (fifo_last.ok()) {
    auto guard = std::move(fifo_last.value());
  }
  if (lru_last.ok()) {
    auto guard = std::move(lru_last.value());
  }
  return Check(fifo_bpm.Close().ok() && lru_bpm.Close().ok(), "策略对照缓冲池关闭应成功") &&
         CloseDb(dm) && stats_ok;
}

bool TestDirtyAndCleanEviction() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }
  std::vector<oursql::page_id_t> pages;
  if (!AllocatePages(dm, 2, &pages)) {
    CloseDb(dm);
    return false;
  }
  dm.ResetIoStatistics();
  {
    oursql::BufferPoolManager bpm(1, &dm);
    {
      auto write_result = bpm.FetchPageWrite(pages[0]);
      if (!Check(write_result.ok(), "写 guard Fetch 应成功")) {
        CloseDb(dm);
        return false;
      }
      auto write_guard = std::move(write_result.value());
      write_guard.Data()[0] = std::byte{0x5A};
      if (!Check(write_guard.MarkDirty().ok(), "显式标脏应成功")) {
        CloseDb(dm);
        return false;
      }
    }
    auto evicted = bpm.FetchPage(pages[1]);
    if (!Check(evicted.ok(), "触发脏页淘汰应成功")) {
      CloseDb(dm);
      return false;
    }
    auto evicted_guard = std::move(evicted.value());
    if (!Check(dm.GetWriteCount() == 1, "脏页淘汰前应恰好写回一次")) {
      CloseDb(dm);
      return false;
    }
  }
  if (!CloseDb(dm)) {
    return false;
  }

  oursql::DiskManager reopened;
  if (!OpenDb(reopened, temp.path)) {
    return false;
  }
  auto persisted = reopened.ReadPage(pages[0]);
  const bool persisted_ok = Check(persisted.ok() && persisted.value().Data()[0] == std::byte{0x5A},
                                  "脏页淘汰后重启应读到修改");
  if (!CloseDb(reopened) || !persisted_ok) {
    return false;
  }

  oursql::DiskManager clean_dm;
  TempDb clean_temp;
  if (!OpenDb(clean_dm, clean_temp.path)) {
    return false;
  }
  std::vector<oursql::page_id_t> clean_pages;
  if (!AllocatePages(clean_dm, 2, &clean_pages)) {
    CloseDb(clean_dm);
    return false;
  }
  clean_dm.ResetIoStatistics();
  oursql::BufferPoolManager clean_bpm(1, &clean_dm);
  {
    auto clean_first = clean_bpm.FetchPage(clean_pages[0]);
    if (!Check(clean_first.ok(), "干净页首次 Fetch 应成功")) {
      CloseDb(clean_dm);
      return false;
    }
    auto clean_first_guard = std::move(clean_first.value());
  }
  auto clean_second = clean_bpm.FetchPage(clean_pages[1]);
  if (!Check(clean_second.ok(), "干净页淘汰应成功")) {
    CloseDb(clean_dm);
    return false;
  }
  auto clean_second_guard = std::move(clean_second.value());
  const bool clean_ok = Check(clean_dm.GetWriteCount() == 0, "干净页淘汰不应产生磁盘写");
  return CloseDb(clean_dm) && clean_ok;
}

bool TestGuardAndExplicitErrors() {
  TempDb temp;
  oursql::DiskManager dm;
  if (!OpenDb(dm, temp.path)) {
    return false;
  }
  std::vector<oursql::page_id_t> pages;
  if (!AllocatePages(dm, 1, &pages)) {
    CloseDb(dm);
    return false;
  }
  oursql::BufferPoolManager bpm(1, &dm);
  {
    auto result = bpm.FetchPage(pages[0]);
    if (!Check(result.ok(), "guard 测试 Fetch 应成功")) {
      CloseDb(dm);
      return false;
    }
    auto guard = std::move(result.value());
    auto delete_status = bpm.DeletePage(pages[0]);
    if (!Check(!delete_status.ok() && delete_status.code() == oursql::ErrorCode::InvalidArgument,
               "被 pin 页面删除应返回 InvalidArgument")) {
      CloseDb(dm);
      return false;
    }
  }
  if (!Check(bpm.UnpinPage(pages[0], false).code() == oursql::ErrorCode::InvalidArgument,
             "重复 Unpin 应返回 InvalidArgument")) {
    CloseDb(dm);
    return false;
  }
  if (!Check(bpm.FlushPage(99).code() == oursql::ErrorCode::NotFound,
             "Flush 不存在页应返回 NotFound")) {
    CloseDb(dm);
    return false;
  }
  oursql::BufferPoolManager write_through_bpm(1, &dm, oursql::ReplacementPolicy::FIFO,
                                               oursql::FlushPolicy::WriteThrough);
  const bool not_implemented =
      Check(write_through_bpm.GetInitStatus().code() == oursql::ErrorCode::NotImplemented,
            "WriteThrough 应明确返回 NotImplemented");
  return CloseDb(dm) && not_implemented;
}

bool TestPinnedDeletePreservesPageAndAllowsReuse() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!OpenDb(disk, temp.path)) return false;
  oursql::BufferPoolManager pool(2, &disk);
  oursql::page_id_t page_id = oursql::INVALID_PAGE_ID;
  {
    auto created = pool.NewPage();
    if (!Check(created.ok(), "NewPage should succeed for pinned delete test")) return false;
    auto guard = std::move(created.value());
    page_id = guard.PageId();
    guard.Data()[100] = std::byte{0x5A};
    guard.Data()[101] = std::byte{0xC3};
    if (!Check(guard.MarkDirty().ok(), "Pinned page marker should become dirty")) return false;

    const auto rejected = pool.DeletePage(page_id);
    if (!Check(rejected.code() == oursql::ErrorCode::InvalidArgument,
               "Deleting a pinned page must return InvalidArgument")) return false;
    if (!Check(rejected.message().find("被 pin 的页面不能删除") != std::string::npos,
               "Pinned delete error should explain the stable contract")) return false;
    if (!Check(guard.Data()[100] == std::byte{0x5A} &&
                   guard.Data()[101] == std::byte{0xC3},
               "Rejected delete must preserve page bytes and the live guard")) return false;
  }

  {
    auto fetched = pool.FetchPage(page_id);
    if (!Check(fetched.ok(), "Rejected page should remain fetchable")) return false;
    auto guard = std::move(fetched.value());
    if (!Check(guard.Data()[100] == std::byte{0x5A} &&
                   guard.Data()[101] == std::byte{0xC3},
               "Rejected page should retain its marker")) return false;
  }
  if (!Check(pool.DeletePage(page_id).ok(), "Delete should succeed after guards release")) {
    return false;
  }
  {
    auto reused = pool.NewPage();
    if (!Check(reused.ok() && reused.value().PageId() == page_id,
               "NewPage should immediately reuse the deleted page id")) return false;
  }
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Pinned delete test should close cleanly");
}

bool TestBufferPoolDeletePersistsFreeListAcrossRestart() {
  TempDb temp;
  oursql::page_id_t released_page = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk;
    if (!OpenDb(disk, temp.path)) return false;
    oursql::BufferPoolManager pool(2, &disk);
    {
      auto created = pool.NewPage();
      if (!Check(created.ok(), "NewPage should succeed before restart")) return false;
      released_page = created.value().PageId();
    }
    if (!Check(pool.DeletePage(released_page).ok(),
               "Unpinned page should be deleted before restart")) return false;
    if (!Check(pool.Close().ok() && disk.Close().ok(),
               "Free-list persistence setup should close cleanly")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!OpenDb(disk, temp.path)) return false;
    oursql::BufferPoolManager pool(2, &disk);
    auto reused = pool.NewPage();
    if (!Check(reused.ok() && reused.value().PageId() == released_page,
               "Restarted NewPage should reuse the persisted free page")) return false;
    return Check(pool.Close().ok() && disk.Close().ok(),
                 "Free-list restart test should close cleanly");
  }
}

}  // namespace

int main() {
  int failures = 0;

  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) {
      std::cout << "[PASS] " << name << '\n';
      return;
    }
    ++failures;
  };

  run("New allocation sequence", &TestNewAllocationSequence);
  run("Persist/read/write and isolation", &TestPersistReadWriteAndIsolation);
  run("Reuse freed page after restart", &TestReuseFreedPageAfterRestart);
  run("Error handling and statistics", &TestErrorHandlingAndStatistics);
  run("Catalog head persistence", &TestCatalogHeadPersistence);
  run("FIFO and LRU replacers", &TestReplacerPolicies);
  run("Buffer pool hit and pinned failure", &TestBufferPoolHitAndPinnedFailure);
  run("FIFO and LRU eviction order", &TestFifoAndLruEvictionOrder);
  run("Free frame list and policy contrast", &TestFreeFrameListAndPolicyContrast);
  run("Dirty and clean eviction", &TestDirtyAndCleanEviction);
  run("Page guards and explicit errors", &TestGuardAndExplicitErrors);
  run("Pinned delete preserves page and allows reuse",
      &TestPinnedDeletePreservesPageAndAllowsReuse);
  run("Buffer pool delete persists free list across restart",
      &TestBufferPoolDeletePersistsFreeListAcrossRestart);

  if (failures == 0) {
    std::cout << "All storage tests passed\n";
    return 0;
  }

  std::cerr << failures << " storage test(s) failed\n";
  return 1;
}
