#include "oursql/execution/database_engine.h"
#include "oursql/recovery/recovery_manager.h"
#include "oursql/storage/log_manager.h"
#include "oursql/storage/storage.h"
#include "oursql/transaction/transaction_manager.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

namespace {

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "FAIL: " << message << '\n';
  return condition;
}

std::filesystem::path BuildPath(const char *argv0, const char *name) {
  std::error_code ec;
  const auto executable = std::filesystem::absolute(argv0, ec);
  const auto directory = ec ? std::filesystem::current_path() : executable.parent_path();
  return directory / name;
}

int CrashChild(const std::string &mode, const std::filesystem::path &database) {
  const bool uncommitted = mode == "uncommitted" || mode == "index-uncommitted";
  oursql::DatabaseEngine engine(database, uncommitted ? 1 : 3);
  if (!engine.GetInitStatus().ok()) return 2;
  auto begin = engine.ExecuteSql("BEGIN;");
  if (!begin.ok()) return 3;
  const char *value = mode == "index-uncommitted"
                          ? "3, 'CrashIndexUndo'"
                          : mode == "uncommitted" ? "3, 'CrashUndo'" : "4, 'CrashRedo'";
  auto insert = engine.ExecuteSql(std::string("INSERT INTO student VALUES(") + value + ");");
  if (!insert.ok()) return 4;
  if (uncommitted) {
    // Force WAL-before-data and make recovery undo a page that reached disk.
    if (!engine.Flush().ok()) return 5;
  } else {
    auto commit = engine.ExecuteSql("COMMIT;");
    if (!commit.ok()) return 6;
  }
  std::_Exit(0);
}

bool TestLogManager(const std::filesystem::path &database) {
  const auto wal_database = database.string() + ".log-only";
  std::filesystem::remove(wal_database + ".wal");
  oursql::LogManager log;
  if (!Check(log.Open(wal_database).ok(), "LogManager open")) return false;
  oursql::LogRecord begin;
  begin.type = oursql::LogRecordType::Begin;
  begin.txn_id = 7;
  auto first = log.Append(begin);
  if (!Check(first.ok() && first.value() == 1, "first WAL LSN")) return false;
  oursql::LogRecord update;
  update.type = oursql::LogRecordType::PageUpdate;
  update.txn_id = 7;
  update.prev_lsn = first.value();
  update.page_id = 2;
  update.before_image.assign(oursql::Page::kSize, std::byte{0x11});
  update.after_image.assign(oursql::Page::kSize, std::byte{0x22});
  auto second = log.Append(update);
  if (!Check(second.ok() && second.value() == 2, "second WAL LSN")) return false;
  if (!Check(log.Flush(second.value()).ok(), "WAL flush")) return false;
  auto records = log.Scan();
  if (!Check(records.ok() && records.value().size() == 2, "WAL scan count")) return false;
  if (!Check(records.value()[1].before_image.size() == oursql::Page::kSize &&
                 records.value()[1].after_image[0] == std::byte{0x22},
             "WAL page images")) return false;
  if (!Check(log.Close().ok(), "WAL close")) return false;

  // A partial final record is a recoverable torn tail.
  {
    std::ofstream tail(log.path(), std::ios::binary | std::ios::app);
    tail.write("tail", 4);
  }
  oursql::LogManager reopened;
  if (!Check(reopened.Open(wal_database).ok(), "WAL torn tail repair")) return false;
  auto repaired = reopened.Scan();
  if (!Check(repaired.ok() && repaired.value().size() == 2, "repaired WAL scan")) return false;
  if (!Check(reopened.Truncate().ok() && reopened.Close().ok(), "WAL truncate")) return false;
  std::filesystem::remove(wal_database + ".wal");
  return true;
}

bool CorruptWalByte(const std::filesystem::path &wal_path, std::size_t offset) {
  std::fstream file(wal_path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file.is_open()) return false;
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  char byte = 0;
  file.read(&byte, 1);
  if (!file) return false;
  byte = static_cast<char>(byte ^ 0x01);
  file.clear();
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file.write(&byte, 1);
  file.flush();
  return static_cast<bool>(file);
}

bool MakeThreeRecordWal(const std::filesystem::path &database) {
  std::filesystem::remove(database.string() + ".wal");
  oursql::LogManager log;
  if (!log.Open(database).ok()) return false;
  oursql::lsn_t last_lsn = oursql::kInvalidLsn;
  for (oursql::txn_id_t txn_id = 1; txn_id <= 3; ++txn_id) {
    oursql::LogRecord record;
    record.type = oursql::LogRecordType::Begin;
    record.txn_id = txn_id;
    auto appended = log.Append(record);
    if (!appended.ok()) return false;
    last_lsn = appended.value();
  }
  return log.Flush(last_lsn).ok() && log.Close().ok();
}

bool MakeBrokenPrevLsnWal(const std::filesystem::path &database) {
  std::filesystem::remove(database.string() + ".wal");
  oursql::LogManager log;
  if (!log.Open(database).ok()) return false;
  oursql::LogRecord first;
  first.type = oursql::LogRecordType::Begin;
  first.txn_id = 7;
  auto first_lsn = log.Append(first);
  if (!first_lsn.ok()) return false;
  oursql::LogRecord broken;
  broken.type = oursql::LogRecordType::Commit;
  broken.txn_id = 7;
  broken.prev_lsn = oursql::kInvalidLsn;
  auto broken_lsn = log.Append(broken);
  if (!broken_lsn.ok()) return false;
  oursql::LogRecord later;
  later.type = oursql::LogRecordType::Begin;
  later.txn_id = 8;
  auto later_lsn = log.Append(later);
  if (!later_lsn.ok()) return false;
  return log.Flush(later_lsn.value()).ok() && log.Close().ok();
}

bool TestWalCorruption(const std::filesystem::path &database) {
  const auto wal_path = std::filesystem::path(database.string() + ".wal");
  const auto cleanup = [&] {
    std::filesystem::remove(database);
    std::filesystem::remove(wal_path);
  };

  // A checksum failure in the last complete record is treated as a torn tail.
  if (!Check(MakeThreeRecordWal(database), "CRC tail setup")) return false;
  if (!Check(CorruptWalByte(wal_path, 2 * 48 + 12), "CRC checksum field corruption")) {
    cleanup();
    return false;
  }
  oursql::LogManager tail_checksum;
  if (!Check(tail_checksum.Open(database).ok(), "CRC checksum tail should be repairable")) {
    cleanup();
    return false;
  }
  auto tail_records = tail_checksum.Scan();
  const bool tail_ok = Check(tail_records.ok() && tail_records.value().size() == 2,
                              "CRC checksum tail record count") &&
                       Check(std::filesystem::file_size(wal_path) == 2 * 48,
                             "CRC checksum tail truncation offset") &&
                       Check(tail_checksum.Close().ok(), "CRC checksum tail close");
  cleanup();
  if (!tail_ok) return false;

  // A non-checksum header byte in the final record is also a recoverable tail.
  if (!Check(MakeThreeRecordWal(database), "CRC header tail setup") ||
      !Check(CorruptWalByte(wal_path, 2 * 48 + 16), "CRC header tail corruption")) {
    cleanup();
    return false;
  }
  oursql::LogManager tail_header;
  if (!Check(tail_header.Open(database).ok(), "CRC header tail should be repairable")) {
    cleanup();
    return false;
  }
  auto header_records = tail_header.Scan();
  const bool header_ok = Check(header_records.ok() && header_records.value().size() == 2,
                                "CRC header tail record count") &&
                         Check(std::filesystem::file_size(wal_path) == 2 * 48,
                               "CRC header tail truncation offset") &&
                         Check(tail_header.Close().ok(), "CRC header tail close");
  cleanup();
  if (!header_ok) return false;

  // Corruption before the physical tail must fail without truncating later records.
  if (!Check(MakeThreeRecordWal(database), "CRC middle setup") ||
      !Check(CorruptWalByte(wal_path, 48 + 16), "CRC middle corruption")) {
    cleanup();
    return false;
  }
  oursql::LogManager middle;
  const bool rejected = Check(!middle.Open(database).ok(),
                              "CRC middle corruption must be rejected") &&
                        Check(std::filesystem::file_size(wal_path) == 3 * 48,
                              "CRC middle corruption must not truncate WAL");
  (void)middle.Close();
  cleanup();
  if (!rejected) return false;

  if (!Check(MakeBrokenPrevLsnWal(database), "WAL prev_lsn setup")) return false;
  oursql::LogManager broken_chain;
  const bool chain_rejected = Check(!broken_chain.Open(database).ok(),
                                    "invalid WAL prev_lsn chain must be rejected") &&
                              Check(std::filesystem::file_size(wal_path) == 3 * 48,
                                    "invalid WAL prev_lsn chain must not truncate WAL");
  (void)broken_chain.Close();
  cleanup();
  return chain_rejected;
}

bool TestTransactionIdSeed(const std::filesystem::path &database) {
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(database).ok(), "transaction id database open") ||
      !Check(log.Open(database).ok(), "transaction id WAL open")) {
    return false;
  }
  oursql::LogRecord historical;
  historical.type = oursql::LogRecordType::Begin;
  historical.txn_id = 41;
  auto historical_lsn = log.Append(historical);
  if (!Check(historical_lsn.ok() && log.Flush(historical_lsn.value()).ok(),
             "transaction id historical WAL")) {
    return false;
  }
  auto seed = log.GetNextTransactionIdSeed();
  if (!Check(seed.ok() && seed.value() == 42, "transaction id seed from WAL")) return false;

  oursql::BufferPoolManager pool(1, &disk);
  oursql::TransactionManager transactions(&log, &pool, &disk);
  if (!Check(transactions.SetNextTxnId(seed.value()).ok(), "set transaction id seed")) {
    return false;
  }
  auto begin = transactions.Begin(true);
  if (!Check(begin.ok() && begin.value()->id() == 42, "transaction id must not be reused")) {
    return false;
  }
  if (!Check(!transactions.SetNextTxnId(99).ok(),
             "transaction id seed cannot change while active") ||
      !Check(transactions.Rollback().ok(), "transaction id seed rollback")) {
    return false;
  }
  const bool closed = Check(pool.Close().ok(), "transaction id pool close") &&
                      Check(log.Close().ok(), "transaction id WAL close") &&
                      Check(disk.Close().ok(), "transaction id database close");
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  return closed;
}

bool HasDirtyPage(const oursql::BufferPoolManager &pool, oursql::page_id_t page_id) {
  for (const auto &snapshot : pool.GetFrameSnapshots()) {
    if (snapshot.page_id.has_value() && snapshot.page_id.value() == page_id) {
      return snapshot.is_dirty;
    }
  }
  return false;
}

bool TestWalBeforeDataFailures(const std::filesystem::path &database) {
  const auto cleanup = [&] {
    std::filesystem::remove(database);
    std::filesystem::remove(database.string() + ".wal");
  };

  // FlushPage must stop at WAL when the WAL flush fails.
  {
    cleanup();
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "FlushPage fault database open") ||
        !Check(log.Open(database).ok(), "FlushPage fault WAL open")) return false;
    oursql::BufferPoolManager pool(1, &disk);
    if (!Check(pool.SetLogManager(&log).ok(), "FlushPage fault WAL binding")) return false;
    oursql::TransactionManager transactions(&log, &pool, &disk);
    auto begin = transactions.Begin(true);
    auto page = disk.AllocatePage();
    if (!Check(begin.ok() && page.ok(), "FlushPage fault setup")) return false;
    auto guard = pool.FetchPageWrite(page.value(), begin.value());
    if (!Check(guard.ok(), "FlushPage fault write fetch")) return false;
    guard.value().Data()[0] = std::byte{0x41};
    if (!Check(guard.value().MarkDirty().ok() && guard.value().Release().ok(),
               "FlushPage fault write release")) return false;
    disk.ResetIoStatistics();
    log.FailNextFlushForTesting();
    auto failed = pool.FlushPage(page.value());
    if (!Check(!failed.ok() && disk.GetWriteCount() == 0 && HasDirtyPage(pool, page.value()),
               "FlushPage must preserve dirty page after WAL failure")) return false;
    if (!Check(pool.FlushPage(page.value()).ok(), "FlushPage retry") ||
        !Check(transactions.Rollback().ok(), "FlushPage fault rollback") ||
        !Check(pool.DeletePage(page.value()).ok(), "FlushPage fault cleanup")) return false;
    if (!Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "FlushPage fault close")) return false;
  }

  // A dirty eviction has the same ordering guarantee and keeps its victim on failure.
  {
    cleanup();
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "eviction fault database open") ||
        !Check(log.Open(database).ok(), "eviction fault WAL open")) return false;
    oursql::BufferPoolManager pool(1, &disk);
    if (!Check(pool.SetLogManager(&log).ok(), "eviction fault WAL binding")) return false;
    oursql::TransactionManager transactions(&log, &pool, &disk);
    auto begin = transactions.Begin(true);
    auto first = disk.AllocatePage();
    auto second = disk.AllocatePage();
    if (!Check(begin.ok() && first.ok() && second.ok(), "eviction fault setup")) return false;
    auto first_guard = pool.FetchPageWrite(first.value(), begin.value());
    if (!Check(first_guard.ok(), "eviction fault first fetch")) return false;
    first_guard.value().Data()[0] = std::byte{0x52};
    if (!Check(first_guard.value().MarkDirty().ok() && first_guard.value().Release().ok(),
               "eviction fault first release")) return false;
    disk.ResetIoStatistics();
    log.FailNextFlushForTesting();
    auto failed = pool.FetchPage(second.value());
    if (!Check(!failed.ok() && disk.GetWriteCount() == 0 && HasDirtyPage(pool, first.value()),
               "dirty eviction must preserve victim after WAL failure")) return false;
    (void)pool.FetchPage(second.value());  // consume the one-shot load failure.
    auto retry = pool.FetchPage(second.value());
    if (!Check(retry.ok(), "dirty eviction retry")) return false;
    {
      auto retry_guard = std::move(retry.value());
      if (!Check(retry_guard.IsValid(), "dirty eviction retry guard")) return false;
    }
    if (!Check(transactions.Rollback().ok(), "eviction fault rollback")) return false;
    if (!Check(pool.DeletePage(first.value()).ok() && pool.DeletePage(second.value()).ok(),
               "eviction fault cleanup") ||
        !Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "eviction fault close")) return false;
  }

  // WriteThrough must report a failed final guard release and retain the dirty frame.
  {
    cleanup();
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "WriteThrough fault database open") ||
        !Check(log.Open(database).ok(), "WriteThrough fault WAL open")) return false;
    oursql::BufferPoolManager pool(1, &disk, oursql::ReplacementPolicy::FIFO,
                                   oursql::FlushPolicy::WriteThrough);
    if (!Check(pool.SetLogManager(&log).ok(), "WriteThrough fault WAL binding")) return false;
    oursql::TransactionManager transactions(&log, &pool, &disk);
    auto begin = transactions.Begin(true);
    auto page = disk.AllocatePage();
    if (!Check(begin.ok() && page.ok(), "WriteThrough fault setup")) return false;
    auto guard = pool.FetchPageWrite(page.value(), begin.value());
    if (!Check(guard.ok(), "WriteThrough fault fetch")) return false;
    guard.value().Data()[0] = std::byte{0x63};
    if (!Check(guard.value().MarkDirty().ok(), "WriteThrough fault mark dirty")) return false;
    disk.ResetIoStatistics();
    log.FailNextFlushForTesting();
    const auto release = guard.value().Release();
    if (!Check(!release.ok() && disk.GetWriteCount() == 0 && HasDirtyPage(pool, page.value()),
               "WriteThrough must preserve dirty page after WAL failure")) return false;
    if (!Check(transactions.Rollback().ok(), "WriteThrough fault rollback") ||
        !Check(pool.DeletePage(page.value()).ok(), "WriteThrough fault cleanup") ||
        !Check(pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "WriteThrough fault close")) return false;
  }

  // FlushAllPages and Close both use the same WAL-before-data helper and are retryable.
  for (const bool close_path : {false, true}) {
    cleanup();
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "bulk flush fault database open") ||
        !Check(log.Open(database).ok(), "bulk flush fault WAL open")) return false;
    oursql::BufferPoolManager pool(1, &disk);
    if (!Check(pool.SetLogManager(&log).ok(), "bulk flush fault WAL binding")) return false;
    oursql::TransactionManager transactions(&log, &pool, &disk);
    auto begin = transactions.Begin(true);
    auto page = disk.AllocatePage();
    if (!Check(begin.ok() && page.ok(), "bulk flush fault setup")) return false;
    auto guard = pool.FetchPageWrite(page.value(), begin.value());
    if (!Check(guard.ok(), "bulk flush fault fetch")) return false;
    guard.value().Data()[0] = std::byte{0x74};
    if (!Check(guard.value().MarkDirty().ok() && guard.value().Release().ok(),
               "bulk flush fault release")) return false;
    disk.ResetIoStatistics();
    log.FailNextFlushForTesting();
    const auto failed = close_path ? pool.Close() : pool.FlushAllPages();
    if (!Check(!failed.ok() && disk.GetWriteCount() == 0 && HasDirtyPage(pool, page.value()),
               close_path ? "Close must preserve dirty page after WAL failure"
                          : "FlushAllPages must preserve dirty page after WAL failure")) {
      return false;
    }
    const auto retry = close_path ? pool.Close() : pool.FlushAllPages();
    if (!Check(retry.ok(), close_path ? "Close retry" : "FlushAllPages retry")) return false;
    if (!Check(transactions.Commit().ok(), "bulk flush fault commit") ||
        (!close_path && !Check(pool.DeletePage(page.value()).ok(), "bulk flush fault cleanup"))) {
      return false;
    }
    if (!close_path && !Check(pool.Close().ok(), "bulk flush pool close")) return false;
    if (!Check(log.Close().ok() && disk.Close().ok(), "bulk flush fault close")) return false;
  }
  cleanup();
  return true;
}

bool TestTransactionalPageLifecycle(const std::filesystem::path &database) {
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");

  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(database).ok(), "page lifecycle database open") ||
      !Check(log.Open(database).ok(), "page lifecycle WAL open")) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(3, &disk);
  if (!Check(buffer_pool.GetInitStatus().ok(), "page lifecycle buffer pool") ||
      !Check(buffer_pool.SetLogManager(&log).ok(), "page lifecycle WAL binding")) {
    return false;
  }
  oursql::TransactionManager transactions(&log, &buffer_pool, &disk);

  auto begin = transactions.Begin(true);
  if (!Check(begin.ok(), "page lifecycle begin") ) return false;
  auto first = buffer_pool.NewPage(begin.value());
  if (!Check(first.ok(), "transactional page allocation") ) return false;
  const auto first_page = first.value().PageId();
  if (!Check(!transactions.Commit().ok(), "commit must reject a held write guard")) return false;
  if (!Check(first.value().Release().ok(), "release allocation guard")) return false;
  if (!Check(transactions.Rollback().ok(), "rollback page allocation")) return false;

  auto reused = buffer_pool.NewPage();
  if (!Check(reused.ok() && reused.value().PageId() == first_page,
             "rolled back allocation should return to the free list")) {
    return false;
  }
  if (!Check(reused.value().Release().ok(), "release reused page")) return false;

  auto commit_begin = transactions.Begin(true);
  if (!Check(commit_begin.ok(), "deferred free begin")) return false;
  auto committed_page = buffer_pool.NewPage(commit_begin.value());
  if (!Check(committed_page.ok(), "deferred free allocation")) return false;
  const auto committed_page_id = committed_page.value().PageId();
  if (!Check(committed_page.value().Release().ok(), "release deferred free allocation")) return false;
  if (!Check(buffer_pool.DeletePage(committed_page_id, commit_begin.value()).ok(),
             "defer page free")) {
    return false;
  }
  if (!Check(buffer_pool.DeletePage(committed_page_id, commit_begin.value()).code() ==
                 oursql::ErrorCode::AlreadyExists,
             "duplicate deferred page free should be rejected")) {
    return false;
  }
  auto probe = disk.AllocatePage();
  if (!Check(probe.ok() && probe.value() != committed_page_id,
             "deferred free must not reach the free list before commit")) {
    return false;
  }
  if (!Check(disk.DeallocatePage(probe.value()).ok(), "release deferred free probe")) return false;
  if (!Check(transactions.Commit().ok(), "commit deferred page free")) return false;

  auto committed_reuse = buffer_pool.NewPage();
  if (!Check(committed_reuse.ok() && committed_reuse.value().PageId() == committed_page_id,
             "committed page free should be reusable")) {
    return false;
  }
  if (!Check(committed_reuse.value().Release().ok(), "release committed reused page")) return false;

  // A clean checkpoint must clear the in-memory page LSN. Otherwise a later
  // non-transactional compatibility write would try to flush a truncated WAL LSN.
  auto lsn_begin = transactions.Begin(true);
  if (!Check(lsn_begin.ok(), "page LSN regression begin")) return false;
  auto lsn_page = buffer_pool.NewPage(lsn_begin.value());
  if (!Check(lsn_page.ok(), "page LSN regression allocation")) return false;
  const auto lsn_page_id = lsn_page.value().PageId();
  lsn_page.value().Data()[0] = std::byte{0x5a};
  if (!Check(lsn_page.value().MarkDirty().ok() && lsn_page.value().Release().ok(),
             "page LSN regression write")) {
    return false;
  }
  if (!Check(transactions.Commit().ok(), "page LSN regression commit") ||
      !Check(buffer_pool.FlushAllPages().ok(), "page LSN regression flush") ||
      !Check(log.Truncate().ok(), "page LSN regression WAL truncate")) {
    return false;
  }
  auto compatibility_write = buffer_pool.FetchPageWrite(lsn_page_id);
  if (!Check(compatibility_write.ok(), "compatibility write after WAL truncate")) return false;
  compatibility_write.value().Data()[0] = std::byte{0x6b};
  if (!Check(compatibility_write.value().MarkDirty().ok() &&
                 compatibility_write.value().Release().ok() &&
                 buffer_pool.FlushAllPages().ok(),
             "compatibility write should flush after WAL truncate")) {
    return false;
  }
  if (!Check(buffer_pool.Close().ok(), "close page lifecycle buffer pool") ||
      !Check(log.Close().ok(), "close page lifecycle WAL") ||
      !Check(disk.Close().ok(), "close page lifecycle database")) {
    return false;
  }
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  return true;
}

bool TestCommittedPageFreeRecovery(const std::filesystem::path &database) {
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");

  oursql::page_id_t freed_page = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "PageFree recovery database open") ||
        !Check(log.Open(database).ok(), "PageFree recovery WAL open")) {
      return false;
    }
    auto allocated = disk.AllocatePage();
    if (!Check(allocated.ok(), "PageFree recovery page allocation")) return false;
    freed_page = allocated.value();

    oursql::LogRecord begin;
    begin.type = oursql::LogRecordType::Begin;
    begin.txn_id = 91;
    auto begin_lsn = log.Append(begin);
    if (!Check(begin_lsn.ok(), "PageFree recovery begin log")) return false;
    oursql::LogRecord free_record;
    free_record.type = oursql::LogRecordType::PageFree;
    free_record.txn_id = 91;
    free_record.prev_lsn = begin_lsn.value();
    free_record.page_id = freed_page;
    auto free_lsn = log.Append(free_record);
    if (!Check(free_lsn.ok(), "PageFree recovery free log")) return false;
    oursql::LogRecord commit;
    commit.type = oursql::LogRecordType::Commit;
    commit.txn_id = 91;
    commit.prev_lsn = free_lsn.value();
    auto commit_lsn = log.Append(commit);
    if (!Check(commit_lsn.ok() && log.Flush(commit_lsn.value()).ok(),
               "PageFree recovery commit durable")) {
      return false;
    }
    if (!Check(log.Close().ok() && disk.Close().ok(), "PageFree recovery crash setup close")) {
      return false;
    }
  }

  {
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(database).ok(), "PageFree recovery reopen database") ||
        !Check(log.Open(database).ok(), "PageFree recovery reopen WAL")) {
      return false;
    }
    oursql::RecoveryManager recovery(&log, &disk);
    if (!Check(recovery.Recover().ok(), "PageFree recovery redo")) return false;
    auto reused = disk.AllocatePage();
    if (!Check(reused.ok() && reused.value() == freed_page,
               "committed PageFree should be reusable after recovery")) {
      return false;
    }
    if (!Check(disk.DeallocatePage(reused.value()).ok(), "PageFree recovery cleanup") ||
        !Check(log.Close().ok() && disk.Close().ok(), "PageFree recovery final close")) {
      return false;
    }
  }
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  return true;
}

bool TestTransactions(const std::filesystem::path &database, const char *argv0) {
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  {
    oursql::DatabaseEngine engine(database, 3);
    if (!Check(engine.GetInitStatus().ok(), "initial transaction engine")) return false;
    if (!Check(engine.ExecuteSql("CREATE TABLE student(id INT, name VARCHAR);").ok(),
               "create student")) return false;
    auto rollback = engine.ExecuteSqlBatch(
        "BEGIN; INSERT INTO student VALUES(1, 'RuntimeUndo'); ROLLBACK;");
    if (!Check(rollback.ok() && rollback.value().size() == 3, "runtime rollback batch")) return false;
    auto empty = engine.ExecuteSql("SELECT * FROM student;");
    if (!Check(empty.ok() && empty.value().rows.empty(), "runtime rollback removed row")) return false;
    auto commit = engine.ExecuteSqlBatch(
        "BEGIN; INSERT INTO student VALUES(2, 'Committed'); COMMIT;");
    if (!Check(commit.ok() && commit.value().size() == 3, "explicit commit batch")) return false;
    if (!Check(engine.ExecuteSql("CREATE UNIQUE INDEX idx_student_id ON student(id);").ok(),
               "create student index")) return false;
    if (!Check(engine.Close().ok(), "close after commit")) return false;
  }
  {
    oursql::DatabaseEngine reopened(database, 3);
    if (!Check(reopened.GetInitStatus().ok(), "catalog restart after commit")) return false;
    auto row = reopened.ExecuteSql("SELECT * FROM student WHERE id = 2;");
    if (!Check(row.ok() && row.value().rows.size() == 1 &&
                   row.value().rows[0][1] == oursql::Value("Committed"),
               "committed row survives restart")) return false;
    if (!Check(reopened.Close().ok(), "close before crash children")) return false;
  }

  const auto executable = std::filesystem::absolute(argv0).string();
  const auto run_child = [&](const char *mode) {
    const std::string child_mode = std::string("--crash-") + mode;
#ifdef _WIN32
    const std::string database_text = database.string();
    const char *arguments[] = {executable.c_str(), child_mode.c_str(), database_text.c_str(),
                               nullptr};
    return _spawnv(_P_WAIT, executable.c_str(), arguments);
#else
    const std::string command = "\"" + executable + "\" " + child_mode + " \"" +
                                database.string() + "\"";
    return std::system(command.c_str());
#endif
  };
  if (!Check(run_child("index-uncommitted") == 0, "index uncommitted crash child")) return false;
  {
    // Recovery is intentionally repeatable before the normal engine opens Catalog.
    oursql::DiskManager recovery_disk;
    oursql::LogManager recovery_log;
    if (!Check(recovery_disk.Open(database).ok(), "standalone recovery database open") ||
        !Check(recovery_log.Open(database).ok(), "standalone recovery WAL open")) {
      return false;
    }
    oursql::RecoveryManager recovery(&recovery_log, &recovery_disk);
    if (!Check(recovery.Recover().ok(), "first standalone recovery") ||
        !Check(recovery.Recover().ok(), "idempotent standalone recovery") ||
        !Check(recovery_log.Close().ok(), "close standalone recovery WAL") ||
        !Check(recovery_disk.Close().ok(), "close standalone recovery database")) {
      return false;
    }
  }
  {
    oursql::DatabaseEngine reopened(database, 3);
    if (!Check(reopened.GetInitStatus().ok(), "restart after uncommitted crash")) return false;
    auto missing = reopened.ExecuteSql("SELECT * FROM student WHERE id = 3;");
    auto explain = reopened.ExecuteSql("EXPLAIN SELECT * FROM student WHERE id = 3;");
    if (!Check(missing.ok() && missing.value().rows.empty() && explain.ok() &&
                   explain.value().rows[0][0].AsVarchar().find("IndexScanPlan") !=
                       std::string::npos,
               "crash recovery must undo Heap and index changes")) return false;
    if (!Check(reopened.Close().ok(), "close after undo recovery")) return false;
  }
  if (!Check(run_child("committed") == 0, "committed crash child")) return false;
  {
    oursql::DatabaseEngine reopened(database, 3);
    if (!Check(reopened.GetInitStatus().ok(), "restart after committed crash")) return false;
    auto present = reopened.ExecuteSql("SELECT * FROM student WHERE id = 4;");
    if (!Check(present.ok() && present.value().rows.size() == 1 &&
                   present.value().rows[0][1] == oursql::Value("CrashRedo"),
               "crash recovery redo")) return false;
    if (!Check(reopened.Close().ok(), "final transaction close")) return false;
  }
  std::filesystem::remove(database);
  std::filesystem::remove(database.string() + ".wal");
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--crash-uncommitted") {
    return CrashChild("uncommitted", argv[2]);
  }
  if (argc == 3 && std::string(argv[1]) == "--crash-index-uncommitted") {
    return CrashChild("index-uncommitted", argv[2]);
  }
  if (argc == 3 && std::string(argv[1]) == "--crash-committed") {
    return CrashChild("committed", argv[2]);
  }
  const auto database = BuildPath(argv[0], "transaction-tests.oursql");
  const auto page_lifecycle_database = BuildPath(argv[0], "transaction-page-tests.oursql");
  const auto page_free_database = BuildPath(argv[0], "transaction-page-free-tests.oursql");
  const auto crc_database = BuildPath(argv[0], "transaction-crc-tests.oursql");
  const auto id_database = BuildPath(argv[0], "transaction-id-tests.oursql");
  const auto fault_database = BuildPath(argv[0], "transaction-wal-fault-tests.oursql");
  bool ok = TestLogManager(database) &&
            TestWalCorruption(crc_database) &&
            TestTransactionIdSeed(id_database) &&
            TestWalBeforeDataFailures(fault_database) &&
            TestTransactionalPageLifecycle(page_lifecycle_database) &&
            TestCommittedPageFreeRecovery(page_free_database) &&
            TestTransactions(database, argv[0]);
  std::cout << (ok ? "transaction tests passed\n" : "transaction tests failed\n");
  return ok ? 0 : 1;
}
