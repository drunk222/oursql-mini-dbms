#include "oursql/storage/storage.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    path = std::filesystem::temp_directory_path() / "oursql_buffer_benchmark.oursql";
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

std::vector<oursql::page_id_t> MakeRequests(const std::string &pattern) {
  constexpr int kPageCount = 32;
  constexpr int kRequests = 128;
  std::vector<oursql::page_id_t> requests;
  requests.reserve(kRequests);
  std::mt19937 generator(20260908U);
  if (pattern == "sequential") {
    for (int i = 0; i < kRequests; ++i) {
      requests.push_back(static_cast<oursql::page_id_t>(1 + i % kPageCount));
    }
  } else if (pattern == "random") {
    std::uniform_int_distribution<int> page(1, kPageCount);
    for (int i = 0; i < kRequests; ++i) {
      requests.push_back(static_cast<oursql::page_id_t>(page(generator)));
    }
  } else {
    std::uniform_int_distribution<int> hot(1, 4);
    std::uniform_int_distribution<int> cold(5, kPageCount);
    std::bernoulli_distribution choose_hot(0.8);
    for (int i = 0; i < kRequests; ++i) {
      requests.push_back(static_cast<oursql::page_id_t>(
          choose_hot(generator) ? hot(generator) : cold(generator)));
    }
  }
  return requests;
}

bool CreateFixture(const std::filesystem::path &path) {
  oursql::DiskManager disk;
  if (!disk.Open(path).ok()) return false;
  for (int i = 0; i < 32; ++i) {
    auto page_id = disk.AllocatePage();
    if (!page_id.ok()) return false;
    oursql::Page page;
    page.Data()[0] = std::byte{static_cast<unsigned char>(i)};
    if (!disk.WritePage(page_id.value(), page).ok()) return false;
  }
  return disk.Close().ok();
}

bool ResetFixture(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return !ec && CreateFixture(path);
}

struct Measurement {
  bool ok{false};
  std::uint64_t accesses{0};
  std::uint64_t hits{0};
  std::uint64_t misses{0};
  double hit_rate{0.0};
  std::uint64_t evictions{0};
  std::uint64_t disk_reads{0};
  std::uint64_t disk_writes{0};
  double elapsed_ms{0.0};
};

bool ExecuteRequests(oursql::BufferPoolManager &buffer_pool,
                     const std::vector<oursql::page_id_t> &requests) {
  for (std::size_t i = 0; i < requests.size(); ++i) {
    if (i % 11 == 0) {
      auto page = buffer_pool.FetchPageWrite(requests[i]);
      if (!page.ok()) return false;
      page.value().Data()[1] = std::byte{static_cast<unsigned char>(i)};
      if (!page.value().MarkDirty().ok()) return false;
    } else {
      auto page = buffer_pool.FetchPage(requests[i]);
      if (!page.ok()) return false;
    }
  }
  return true;
}

Measurement Run(const std::filesystem::path &path, oursql::ReplacementPolicy policy,
                const std::vector<oursql::page_id_t> &requests) {
  oursql::DiskManager disk;
  Measurement result;
  if (!disk.Open(path).ok()) return result;
  oursql::BufferPoolManager buffer_pool(4, &disk, policy, oursql::FlushPolicy::WriteBack);
  if (!buffer_pool.GetInitStatus().ok()) return result;
  const auto start = Clock::now();
  if (!ExecuteRequests(buffer_pool, requests)) return result;
  if (!buffer_pool.Close().ok() || !disk.Close().ok()) return result;
  const auto end = Clock::now();
  result.ok = true;
  result.accesses = buffer_pool.GetAccessCount();
  result.hits = buffer_pool.GetHitCount();
  result.misses = buffer_pool.GetMissCount();
  result.hit_rate = buffer_pool.GetHitRate();
  result.evictions = buffer_pool.GetEvictionCount();
  result.disk_reads = disk.GetReadCount();
  result.disk_writes = disk.GetWriteCount();
  result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  return result;
}

Measurement RunHot(const std::filesystem::path &path, oursql::ReplacementPolicy policy,
                   const std::vector<oursql::page_id_t> &requests) {
  oursql::DiskManager disk;
  Measurement result;
  if (!disk.Open(path).ok()) return result;
  oursql::BufferPoolManager buffer_pool(4, &disk, policy, oursql::FlushPolicy::WriteBack);
  if (!buffer_pool.GetInitStatus().ok()) return result;
  if (!ExecuteRequests(buffer_pool, requests)) return result;
  if (!buffer_pool.FlushAllPages().ok()) return result;
  buffer_pool.ResetStatistics();
  disk.ResetIoStatistics();

  const auto start = Clock::now();
  if (!ExecuteRequests(buffer_pool, requests)) return result;
  if (!buffer_pool.Close().ok() || !disk.Close().ok()) return result;
  const auto end = Clock::now();
  result.ok = true;
  result.accesses = buffer_pool.GetAccessCount();
  result.hits = buffer_pool.GetHitCount();
  result.misses = buffer_pool.GetMissCount();
  result.hit_rate = buffer_pool.GetHitRate();
  result.evictions = buffer_pool.GetEvictionCount();
  result.disk_reads = disk.GetReadCount();
  result.disk_writes = disk.GetWriteCount();
  result.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
  return result;
}

void PrintMeasurement(const std::string &pattern, const std::string &policy,
                      const Measurement &measurement) {
  if (!measurement.ok) {
    std::cout << pattern << ' ' << policy << " ERROR\n";
    return;
  }
  std::cout << pattern << ' ' << policy << " accesses=" << measurement.accesses
            << " hits=" << measurement.hits << " misses=" << measurement.misses
            << " hit_rate=" << std::fixed << std::setprecision(4) << measurement.hit_rate
            << " evictions=" << measurement.evictions << " disk_reads="
            << measurement.disk_reads << " disk_writes=" << measurement.disk_writes
            << " elapsed_ms=" << std::setprecision(3) << measurement.elapsed_ms << '\n';
}

void RunContrast(const std::filesystem::path &path, oursql::ReplacementPolicy policy,
                 const std::string &name) {
  oursql::DiskManager disk;
  if (!disk.Open(path).ok()) {
    std::cout << "contrast " << name << " ERROR\n";
    return;
  }
  oursql::BufferPoolManager buffer_pool(3, &disk, policy, oursql::FlushPolicy::WriteBack);
  if (!buffer_pool.GetInitStatus().ok()) {
    std::cout << "contrast " << name << " ERROR\n";
    return;
  }
  const std::vector<oursql::page_id_t> requests{1, 2, 3, 1, 4};
  for (const auto page_id : requests) {
    auto page = buffer_pool.FetchPage(page_id);
    if (!page.ok()) {
      std::cout << "contrast " << name << " ERROR\n";
      return;
    }
    auto guard = std::move(page.value());
  }
  const auto eviction_log = buffer_pool.GetEvictionLog();
  auto last = buffer_pool.FetchPage(1);
  if (!last.ok()) {
    std::cout << "contrast " << name << " ERROR\n";
    return;
  }
  auto last_guard = std::move(last.value());
  const auto evicted = eviction_log.empty() ? oursql::INVALID_PAGE_ID : eviction_log.front();
  std::cout << "contrast " << name << " sequence=1,2,3,1,4,1"
            << " first_evicted=" << evicted << " hits=" << buffer_pool.GetHitCount()
            << " misses=" << buffer_pool.GetMissCount() << '\n';
  (void)buffer_pool.Close();
  (void)disk.Close();
}

}  // namespace

int main() {
  TempDb temp;
  if (!CreateFixture(temp.path)) {
    std::cerr << "fixture creation failed\n";
    return 1;
  }
  std::cout << "OurSQL buffer benchmark pool=4 requests=128 seed=20260908 write_policy=WriteBack\n";
  for (const std::string pattern : {"sequential", "random", "hotspot"}) {
    const auto requests = MakeRequests(pattern);
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern, "FIFO", Run(temp.path, oursql::ReplacementPolicy::FIFO, requests));
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern, "LRU", Run(temp.path, oursql::ReplacementPolicy::LRU, requests));
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern, "CLOCK", Run(temp.path, oursql::ReplacementPolicy::CLOCK, requests));
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern + "-hot", "FIFO",
                     RunHot(temp.path, oursql::ReplacementPolicy::FIFO, requests));
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern + "-hot", "LRU",
                     RunHot(temp.path, oursql::ReplacementPolicy::LRU, requests));
    if (!ResetFixture(temp.path)) return 1;
    PrintMeasurement(pattern + "-hot", "CLOCK",
                     RunHot(temp.path, oursql::ReplacementPolicy::CLOCK, requests));
  }
  if (!ResetFixture(temp.path)) return 1;
  RunContrast(temp.path, oursql::ReplacementPolicy::FIFO, "FIFO");
  if (!ResetFixture(temp.path)) return 1;
  RunContrast(temp.path, oursql::ReplacementPolicy::LRU, "LRU");
  if (!ResetFixture(temp.path)) return 1;
  RunContrast(temp.path, oursql::ReplacementPolicy::CLOCK, "CLOCK");
  if (!ResetFixture(temp.path)) return 1;
  oursql::DiskManager disk;
  if (!disk.Open(temp.path).ok()) return 1;
  oursql::BufferPoolManager write_through_pool(4, &disk, oursql::ReplacementPolicy::LRU,
                                                oursql::FlushPolicy::WriteThrough);
  std::cout << "write_through_status=" << write_through_pool.GetInitStatus().ToString() << '\n';
  (void)disk.Close();
  return 0;
}
