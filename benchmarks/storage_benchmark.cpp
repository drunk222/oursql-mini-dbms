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

Measurement Run(const std::filesystem::path &path, oursql::ReplacementPolicy policy,
                const std::vector<oursql::page_id_t> &requests) {
  oursql::DiskManager disk;
  Measurement result;
  if (!disk.Open(path).ok()) return result;
  oursql::BufferPoolManager buffer_pool(4, &disk, policy, oursql::FlushPolicy::WriteBack);
  if (!buffer_pool.GetInitStatus().ok()) return result;
  const auto start = Clock::now();
  for (std::size_t i = 0; i < requests.size(); ++i) {
    if (i % 11 == 0) {
      auto page = buffer_pool.FetchPageWrite(requests[i]);
      if (!page.ok()) return result;
      page.value().Data()[1] = std::byte{static_cast<unsigned char>(i)};
      if (!page.value().MarkDirty().ok()) return result;
    } else {
      auto page = buffer_pool.FetchPage(requests[i]);
      if (!page.ok()) return result;
    }
  }
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
    PrintMeasurement(pattern, "FIFO", Run(temp.path, oursql::ReplacementPolicy::FIFO, requests));
    PrintMeasurement(pattern, "LRU", Run(temp.path, oursql::ReplacementPolicy::LRU, requests));
  }
  oursql::DiskManager disk;
  if (!disk.Open(temp.path).ok()) return 1;
  oursql::BufferPoolManager write_through_pool(4, &disk, oursql::ReplacementPolicy::LRU,
                                                oursql::FlushPolicy::WriteThrough);
  std::cout << "write_through_status=" << write_through_pool.GetInitStatus().ToString() << '\n';
  (void)disk.Close();
  return 0;
}
