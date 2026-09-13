#include "oursql/execution/database_engine.h"
#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"
#include "oursql/transaction/transaction_manager.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_storage_contract_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path.string() + ".wal", ec);
  }
};

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool TestDatabaseObservabilityAndRestart() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 2, oursql::ReplacementPolicy::LRU,
                                  oursql::FlushPolicy::WriteBack);
    if (!Check(engine.GetInitStatus().ok(), "engine should open for observability test")) {
      return false;
    }

    const auto setup = engine.ExecuteSqlBatch(
        "CREATE TABLE student(id INT, name VARCHAR);"
        "INSERT INTO student VALUES(1, 'Alice');"
        "INSERT INTO student VALUES(2, 'Bob');");
    if (!Check(setup.ok() && setup.value().size() == 3,
               "engine should execute the observability fixture")) {
      return false;
    }

    const auto statistics = engine.GetStatistics();
    const bool counters_ok =
        Check(statistics.buffer_accesses == statistics.buffer_hits + statistics.buffer_misses,
              "statistics snapshot should satisfy accesses = hits + misses") &&
        Check(statistics.buffer_hit_rate >= 0.0 && statistics.buffer_hit_rate <= 1.0,
              "statistics snapshot should contain a valid hit rate");

    auto snapshots = engine.GetBufferSnapshots();
    if (!Check(snapshots.size() == engine.GetPoolSize(),
               "frame snapshot count should equal the configured pool size")) {
      return false;
    }
    const auto snapshots_before = snapshots;
    snapshots[0].pin_count = 999;
    snapshots[0].is_dirty = !snapshots[0].is_dirty;
    const auto snapshots_after = engine.GetBufferSnapshots();
    const bool snapshot_copy_ok = Check(
        snapshots_after.size() == snapshots_before.size() &&
            snapshots_after[0].pin_count == snapshots_before[0].pin_count &&
            snapshots_after[0].is_dirty == snapshots_before[0].is_dirty,
        "frame snapshots should be independent value copies");

    auto eviction_log = engine.GetEvictionLog();
    const auto eviction_log_before = eviction_log;
    eviction_log.push_back(oursql::INVALID_PAGE_ID);
    const auto eviction_log_after = engine.GetEvictionLog();
    const bool eviction_copy_ok = Check(eviction_log_after == eviction_log_before,
                                        "eviction log should be an independent value copy");

    const bool flush_ok = Check(engine.Flush().ok(), "engine Flush should report success");
    if (!counters_ok || !snapshot_copy_ok || !eviction_copy_ok || !flush_ok) return false;
    if (!Check(engine.Close().ok(), "engine should close after an explicit flush")) return false;
    if (!Check(engine.Close().ok(), "engine Close should be idempotent")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 2);
  if (!Check(reopened.GetInitStatus().ok(), "engine should reopen the flushed database")) {
    return false;
  }
  const auto result = reopened.ExecuteSql("SELECT * FROM student;");
  const bool rows_ok = Check(
      result.ok() && result.value().rows.size() == 2 &&
          result.value().rows[0][0].AsInt() == 1 &&
          result.value().rows[0][1].AsVarchar() == "Alice" &&
          result.value().rows[1][0].AsInt() == 2 &&
          result.value().rows[1][1].AsVarchar() == "Bob",
      "reopened engine should read the persisted rows");
  const bool close_ok = Check(reopened.Close().ok(), "reopened engine should close cleanly");
  return rows_ok && close_ok;
}

bool TestStorageSnapshotsAndConcurrentReads() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "disk manager should open for snapshot test")) {
    return false;
  }

  std::vector<oursql::page_id_t> pages;
  for (int i = 0; i < 6; ++i) {
    auto page = disk.AllocatePage();
    if (!Check(page.ok(), "snapshot test page allocation should succeed")) {
      (void)disk.Close();
      return false;
    }
    pages.push_back(page.value());
  }

  oursql::BufferPoolManager pool(2, &disk);
  for (int i = 0; i < 3; ++i) {
    auto result = pool.FetchPage(pages[static_cast<std::size_t>(i)]);
    if (!Check(result.ok(), "snapshot test fetch should succeed")) {
      (void)pool.Close();
      (void)disk.Close();
      return false;
    }
    auto guard = std::move(result.value());
  }

  const auto eviction_log = pool.GetEvictionLog();
  if (!Check(!eviction_log.empty(), "snapshot test should produce an eviction")) {
    (void)pool.Close();
    (void)disk.Close();
    return false;
  }
  auto eviction_copy = eviction_log;
  eviction_copy.clear();
  const bool log_copy_ok = Check(pool.GetEvictionLog() == eviction_log,
                                 "buffer eviction log should not expose internal storage");

  auto frame_snapshots = pool.GetFrameSnapshots();
  if (!Check(frame_snapshots.size() == 2, "buffer snapshot count should be stable")) {
    (void)pool.Close();
    (void)disk.Close();
    return false;
  }
  const auto frame_snapshots_before = frame_snapshots;
  frame_snapshots[0].pin_count = 100;
  const auto frame_snapshots_after = pool.GetFrameSnapshots();
  const bool frame_copy_ok = Check(
      frame_snapshots_after[0].pin_count == frame_snapshots_before[0].pin_count,
      "buffer frame snapshots should not expose internal storage");

  std::atomic<bool> failed{false};
  std::thread observer([&] {
    for (int i = 0; i < 1000; ++i) {
      const auto snapshots = pool.GetFrameSnapshots();
      const auto log = pool.GetEvictionLog();
      const auto rate = pool.GetHitRate();
      if (snapshots.size() != 2 || log.size() > 1024 || !std::isfinite(rate) || rate < 0.0 ||
          rate > 1.0) {
        failed.store(true);
        return;
      }
      (void)pool.GetAccessCount();
      (void)pool.GetHitCount();
      (void)pool.GetMissCount();
      (void)pool.GetEvictionCount();
    }
  });
  std::thread fetcher([&] {
    for (int i = 0; i < 1000; ++i) {
      auto result = pool.FetchPage(pages[static_cast<std::size_t>(i) % pages.size()]);
      if (!result.ok()) {
        failed.store(true);
        return;
      }
      auto guard = std::move(result.value());
      if (guard.Data() == nullptr) {
        failed.store(true);
        return;
      }
    }
  });
  observer.join();
  fetcher.join();

  const bool concurrent_ok = Check(!failed.load(),
                                   "concurrent statistics and snapshot reads should be safe");
  const bool close_ok = Check(pool.Close().ok() && disk.Close().ok(),
                              "snapshot test resources should close cleanly");
  return log_copy_ok && frame_copy_ok && concurrent_ok && close_ok;
}

bool TestFlushFailureIsReportedAndRetryable() {
  TempDb temp;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(temp.path).ok() && log.Open(temp.path).ok(),
             "flush failure fixture should open database and WAL")) {
    return false;
  }

  oursql::BufferPoolManager pool(1, &disk);
  oursql::TransactionManager transactions(&log, &pool, &disk);
  if (!Check(pool.SetLogManager(&log).ok(), "flush failure fixture should bind WAL")) {
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  auto begin = transactions.Begin(true);
  auto page = pool.NewPage(begin.ok() ? begin.value() : nullptr);
  if (!Check(begin.ok() && page.ok(), "flush failure fixture should create a transaction page")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }
  const auto page_id = page.value().PageId();
  auto guard = std::move(page.value());
  guard.Data()[0] = std::byte{0x5A};
  if (!Check(guard.MarkDirty().ok() && guard.Release().ok(),
             "flush failure fixture should make a dirty page")) {
    (void)pool.Close();
    (void)log.Close();
    (void)disk.Close();
    return false;
  }

  disk.ResetIoStatistics();
  log.FailNextFlushForTesting();
  const auto failed_flush = pool.FlushAllPages();
  const bool failure_ok = Check(!failed_flush.ok() && failed_flush.code() == oursql::ErrorCode::IOError,
                                "FlushAllPages should report a WAL I/O failure");
  const auto after_failure = pool.GetFrameSnapshots();
  const bool dirty_preserved = Check(
      !after_failure.empty() && after_failure[0].page_id == page_id && after_failure[0].is_dirty,
      "a failed flush should preserve the dirty frame for retry");
  const bool no_data_write = Check(disk.GetWriteCount() == 0,
                                   "a failed WAL flush should prevent the data-page write");

  const auto retry = pool.FlushAllPages();
  const bool retry_ok = Check(retry.ok(), "FlushAllPages should be retryable after WAL recovery");
  const bool rollback_ok = Check(transactions.Rollback().ok(),
                                 "flush failure fixture should roll back cleanly");
  const bool close_ok = Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
                              "flush failure fixture should close cleanly");
  return failure_ok && dirty_preserved && no_data_write && retry_ok && rollback_ok && close_ok;
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) {
      std::cout << "[PASS] " << name << '\n';
    } else {
      ++failures;
    }
  };

  run("Database observability and restart", &TestDatabaseObservabilityAndRestart);
  run("Storage snapshots and concurrent reads", &TestStorageSnapshotsAndConcurrentReads);
  run("Flush failure is reported and retryable", &TestFlushFailureIsReportedAndRetryable);

  if (failures == 0) {
    std::cout << "All storage contract tests passed\n";
    return 0;
  }
  std::cerr << failures << " storage contract test(s) failed\n";
  return 1;
}
