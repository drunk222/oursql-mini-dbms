#include "oursql/index/b_plus_tree.h"
#include "oursql/index/index_manager.h"
#include "oursql/storage/log_manager.h"
#include "oursql/transaction/lock_manager.h"
#include "oursql/transaction/transaction_manager.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class TempDatabase {
 public:
  TempDatabase() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_bplus_tree_" + std::to_string(stamp) + ".db");
  }
  ~TempDatabase() {
    std::error_code error;
    std::filesystem::remove(path, error);
    std::filesystem::remove(path.string() + ".wal", error);
  }
  std::filesystem::path path;
};

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool TestEmptyTree() {
  TempDatabase temp;
  oursql::DiskManager disk;
  if (!disk.Open(temp.path).ok()) return false;
  oursql::BufferPoolManager buffer_pool(4, &disk);
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 3, 3);
  auto point = tree.GetValue(42);
  auto range = tree.RangeScan(1, 9);
  return Check(tree.IsEmpty(), "新B+树应为空") &&
         Check(point.ok() && point.value().empty(), "空树等值查询应返回空结果") &&
         Check(range.ok() && range.value().empty(), "空树范围查询应返回空结果");
}

bool TestSplitsLookupAndLeafChain() {
  TempDatabase temp;
  oursql::DiskManager disk;
  if (!disk.Open(temp.path).ok()) return false;
  oursql::BufferPoolManager buffer_pool(4, &disk);
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 3, 3);

  // 逆序插入 20 个 Key，小容量会产生多次叶分裂、内部页分裂和根分裂。
  for (std::int64_t key = 20; key >= 1; --key) {
    auto status = tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
    if (!Check(status.ok(), "逆序插入应成功: " + status.ToString())) return false;
  }
  if (!Check(!tree.IsEmpty(), "插入后B+树不应为空")) return false;

  for (std::int64_t key = 1; key <= 20; ++key) {
    auto found = tree.GetValue(key);
    if (!Check(found.ok() && found.value().size() == 1,
               "每个Key应等值命中一个RID")) return false;
    if (!Check(found.value()[0] == oursql::RID(static_cast<oursql::page_id_t>(key), 1),
               "等值查询应返回对应RID")) return false;
  }

  auto range = tree.RangeScan(5, 15);
  if (!Check(range.ok() && range.value().size() == 11,
             "跨多个叶子页的闭区间应返回11项")) return false;
  for (std::size_t i = 0; i < range.value().size(); ++i) {
    if (!Check(range.value()[i].first == static_cast<std::int64_t>(i + 5),
               "叶链范围查询结果应有序")) return false;
  }
  return true;
}

bool TestDuplicateKeysAndReopen() {
  TempDatabase temp;
  oursql::page_id_t header = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager buffer_pool(5, &disk);
    auto created = oursql::BPlusTree::CreatePersistent(&buffer_pool, 3, 3);
    if (!Check(created.ok(), "持久化B+树创建应成功")) return false;
    auto &tree = *created.value();
    for (oursql::page_id_t page = 1; page <= 8; ++page) {
      auto status = tree.Insert(7, oursql::RID{page, 2});
      if (!Check(status.ok(), "重复Key的不同RID应允许插入")) return false;
    }
    auto duplicate = tree.Insert(7, oursql::RID{1, 2});
    if (!Check(!duplicate.ok() && duplicate.code() == oursql::ErrorCode::AlreadyExists,
               "完全相同的Key/RID应拒绝重复插入")) return false;
    header = tree.GetHeaderPageId();
    if (!Check(buffer_pool.Close().ok(), "第一次关闭应写回全部索引页")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager buffer_pool(5, &disk);
    auto opened = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(opened.ok(), "仅凭元数据页编号应能重新打开B+树")) return false;
    auto &reopened = *opened.value();
    auto values = reopened.GetValue(7);
    if (!Check(values.ok() && values.value().size() == 8,
               "数据库重开后应读到全部重复Key")) return false;
    for (std::size_t i = 0; i < values.value().size(); ++i) {
      if (!Check(values.value()[i] == oursql::RID(static_cast<oursql::page_id_t>(i + 1), 2),
                 "重复Key结果应按RID稳定排序")) return false;
    }
    for (std::int64_t key = 20; key <= 40; ++key) {
      auto status = reopened.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 3});
      if (!Check(status.ok(), "重开后的B+树应能继续插入和分裂")) return false;
    }
    if (!Check(buffer_pool.Close().ok(), "第二次关闭应写回新根和新节点")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager buffer_pool(5, &disk);
    auto opened = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(opened.ok(), "第二次重新打开B+树应成功")) return false;
    auto range = opened.value()->RangeScan(20, 40);
    if (!Check(range.ok() && range.value().size() == 21,
               "根再次分裂后持久化范围查询仍应完整")) return false;
  }
  return true;
}

bool TestDeleteRebalanceAndRootContraction() {
  TempDatabase temp;
  oursql::page_id_t header = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager buffer_pool(3, &disk);
    auto created = oursql::BPlusTree::CreatePersistent(&buffer_pool, 3, 3);
    if (!Check(created.ok(), "删除测试持久化树创建应成功")) return false;
    auto &tree = *created.value();
    header = tree.GetHeaderPageId();
    const std::vector<std::int64_t> order{
        23, 1, 45, 12, 37, 5, 49, 18, 31, 8, 42, 2, 28, 16, 35, 10, 47,
        4, 26, 14, 39, 6, 50, 20, 33, 3, 44, 11, 29, 17, 36, 9, 41, 7,
        48, 15, 27, 19, 34, 13, 40, 21, 30, 22, 38, 24, 43, 25, 46, 32};
    for (const auto key : order) {
      auto status = tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
      if (!Check(status.ok(), "乱序插入删除测试Key应成功")) return false;
    }
    for (std::int64_t key = 1; key <= 49; key += 2) {
      auto status = tree.Remove(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
      if (!Check(status.ok(), "删除奇数Key应完成借位或合并")) return false;
    }
    auto remaining = tree.RangeScan(1, 50);
    if (!Check(remaining.ok() && remaining.value().size() == 25,
               "删除一半后叶链应保留25个偶数Key")) return false;
    for (std::size_t i = 0; i < remaining.value().size(); ++i) {
      if (!Check(remaining.value()[i].first == static_cast<std::int64_t>((i + 1) * 2),
                 "删除合并后范围结果应保持有序")) return false;
    }
    for (std::int64_t key = 2; key <= 50; key += 2) {
      auto status = tree.Remove(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
      if (!Check(status.ok(), "删除剩余Key应成功")) return false;
    }
    if (!Check(tree.IsEmpty(), "删除全部Key后根应收缩为空树")) return false;
    if (!Check(buffer_pool.Close().ok(), "删除测试关闭应成功")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager buffer_pool(3, &disk);
    auto opened = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(opened.ok() && opened.value()->IsEmpty(),
               "重开后应恢复已经删除为空的B+树")) return false;
  }
  return true;
}

bool TestDeleteReclaimsPagesAcrossRestart() {
  TempDatabase temp;
  oursql::page_id_t header = oursql::INVALID_PAGE_ID;
  std::uintmax_t file_size_before_delete = 0;
  std::uintmax_t logical_page_count = 0;
  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "Reclaim test database should open")) return false;
    oursql::BufferPoolManager buffer_pool(4, &disk);
    auto created = oursql::BPlusTree::CreatePersistent(&buffer_pool, 3, 3);
    if (!Check(created.ok(), "Persistent reclaim test tree should be created")) return false;
    auto &tree = *created.value();
    header = tree.GetHeaderPageId();
    for (std::int64_t key = 1; key <= 50; ++key) {
      if (!Check(tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1}).ok(),
                 "Reclaim test insert should succeed")) return false;
    }
    if (!Check(buffer_pool.FlushAllPages().ok(), "Tree pages should flush before measuring")) {
      return false;
    }
    file_size_before_delete = std::filesystem::file_size(temp.path);
    logical_page_count = file_size_before_delete / oursql::Page::kSize;

    for (std::int64_t key = 2; key <= 50; ++key) {
      if (!Check(tree.Remove(key,
                             oursql::RID{static_cast<oursql::page_id_t>(key), 1}).ok(),
                 "Deleting keys 2 through 50 should rebalance the tree")) return false;
    }
    auto retained = tree.GetValue(1);
    auto removed = tree.GetValue(50);
    if (!Check(retained.ok() && retained.value().size() == 1,
               "Key 1 should remain after merges and root contraction")) return false;
    if (!Check(removed.ok() && removed.value().empty(),
               "Deleted keys should not remain searchable")) return false;
    if (!Check(buffer_pool.Close().ok() && disk.Close().ok(),
               "Reclaim test first lifetime should close cleanly")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "Reclaim test database should reopen")) return false;
    oursql::BufferPoolManager buffer_pool(4, &disk);
    auto opened = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(opened.ok(), "Persistent tree should reopen from its header")) return false;
    auto retained = opened.value()->GetValue(1);
    auto removed = opened.value()->GetValue(2);
    if (!Check(retained.ok() && retained.value().size() == 1 &&
                   removed.ok() && removed.value().empty(),
               "Reopened tree should preserve post-delete contents")) return false;

    oursql::page_id_t reused_page = oursql::INVALID_PAGE_ID;
    {
      auto page = buffer_pool.NewPage();
      if (!Check(page.ok(), "NewPage should reuse a merged B+ tree page")) return false;
      reused_page = page.value().PageId();
    }
    if (!Check(reused_page < logical_page_count,
               "Reallocated page should come from the pre-delete page range")) return false;
    if (!Check(std::filesystem::file_size(temp.path) == file_size_before_delete,
               "Reusing a merged page must not grow the database file")) return false;
    return Check(buffer_pool.Close().ok() && disk.Close().ok(),
                 "Reclaim test second lifetime should close cleanly");
  }
}

bool TestTransactionalStructuralChanges() {
  TempDatabase temp;
  oursql::page_id_t header = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk;
    oursql::LogManager log;
    if (!Check(disk.Open(temp.path).ok(), "transactional tree database open") ||
        !Check(log.Open(temp.path).ok(), "transactional tree WAL open")) {
      return false;
    }
    oursql::BufferPoolManager buffer_pool(8, &disk);
    if (!Check(buffer_pool.SetLogManager(&log).ok(), "transactional tree WAL binding")) {
      return false;
    }
    auto created = oursql::BPlusTree::CreatePersistent(&buffer_pool, 3, 3);
    if (!Check(created.ok(), "transactional tree persistent setup")) return false;
    header = created.value()->GetHeaderPageId();
    if (!Check(buffer_pool.FlushAllPages().ok(), "transactional tree setup flush")) return false;
    oursql::TransactionManager transactions(&log, &buffer_pool, &disk);

    auto insert_begin = transactions.Begin(true);
    if (!Check(insert_begin.ok(), "transactional structural insert begin")) return false;
    auto inserting = oursql::BPlusTree::OpenPersistent(&buffer_pool, header,
                                                        insert_begin.value());
    if (!Check(inserting.ok(), "transactional structural insert open")) return false;
    for (std::int64_t key = 1; key <= 40; ++key) {
      if (!Check(inserting.value()->Insert(
                     key, oursql::RID{static_cast<oursql::page_id_t>(key), 1})
                     .ok(),
                 "transactional structural insert")) return false;
    }
    if (!Check(transactions.Commit().ok() && buffer_pool.FlushAllPages().ok(),
               "transactional structural insert commit")) return false;

    auto committed_tree = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(committed_tree.ok(), "committed structural tree reopen")) return false;
    auto committed_range = committed_tree.value()->RangeScan(1, 40);
    if (!Check(committed_range.ok() && committed_range.value().size() == 40,
               "committed split tree contents")) return false;

    auto rollback_begin = transactions.Begin(true);
    if (!Check(rollback_begin.ok(), "transactional structural delete begin")) return false;
    auto deleting = oursql::BPlusTree::OpenPersistent(&buffer_pool, header,
                                                       rollback_begin.value());
    if (!Check(deleting.ok(), "transactional structural delete open")) return false;
    for (std::int64_t key = 1; key <= 40; ++key) {
      auto status = deleting.value()->Remove(
          key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
      if (!Check(status.ok(), "transactional structural delete key " +
                                  std::to_string(key) + ": " + status.ToString())) {
        return false;
      }
    }
    if (!Check(transactions.Rollback().ok(), "transactional structural delete rollback")) {
      return false;
    }
    auto restored_tree = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(restored_tree.ok(), "rolled-back structural tree reopen")) return false;
    auto restored_range = restored_tree.value()->RangeScan(1, 40);
    if (!Check(restored_range.ok() && restored_range.value().size() == 40,
               "rollback must restore merged tree contents")) return false;

    auto final_begin = transactions.Begin(true);
    if (!Check(final_begin.ok(), "transactional structural final delete begin")) return false;
    auto deleting_final = oursql::BPlusTree::OpenPersistent(&buffer_pool, header,
                                                              final_begin.value());
    if (!Check(deleting_final.ok(), "transactional structural final delete open")) return false;
    for (std::int64_t key = 1; key <= 40; ++key) {
      if (!Check(deleting_final.value()->Remove(
                     key, oursql::RID{static_cast<oursql::page_id_t>(key), 1})
                     .ok(),
                 "transactional structural final delete")) return false;
    }
    if (!Check(transactions.Commit().ok() && buffer_pool.FlushAllPages().ok(),
               "transactional structural delete commit")) return false;
    auto empty_tree = oursql::BPlusTree::OpenPersistent(&buffer_pool, header);
    if (!Check(empty_tree.ok() && empty_tree.value()->IsEmpty(),
               "committed root contraction must persist")) return false;
    if (!Check(buffer_pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "transactional structural close")) return false;
  }
  std::filesystem::remove(temp.path);
  std::filesystem::remove(temp.path.string() + ".wal");
  return true;
}

class StartGate {
 public:
  explicit StartGate(std::size_t participants) : participants_(participants) {}
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (++arrived_ == participants_) {
      open_ = true;
      changed_.notify_all();
      return;
    }
    changed_.wait(lock, [&] { return open_; });
  }

 private:
  std::size_t participants_;
  std::size_t arrived_{0};
  bool open_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
};

bool TestTransactionalWriteReleaseFailurePropagates() {
  TempDatabase temp;
  oursql::DiskManager disk;
  oursql::LogManager log;
  if (!Check(disk.Open(temp.path).ok(), "release failure database open") ||
      !Check(log.Open(temp.path).ok(), "release failure WAL open")) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(3, &disk);
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 3, 3);
  const oursql::RID original_rid{1, 1};
  if (!Check(tree.Insert(1, original_rid).ok(),
             "release failure baseline insert")) {
    return false;
  }

  // Deliberately leave the BufferPool without a LogManager binding. The
  // transactional page update must fail when the write guard is released.
  oursql::TransactionManager transactions(&log, &buffer_pool, &disk);
  auto begin = transactions.Begin(true);
  if (!Check(begin.ok(), "release failure transaction begin")) return false;
  const auto insert = tree.Insert(2, oursql::RID{2, 1}, begin.value());
  if (!Check(!insert.ok(),
             "B+ tree must propagate transactional write guard release failure") ||
      !Check(begin.value()->state() == oursql::TransactionState::Failed,
             "WAL release failure must mark the transaction Failed")) {
    return false;
  }
  if (!Check(transactions.Rollback().ok(), "release failure rollback")) return false;
  return Check(buffer_pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "release failure resources close");
}

bool TestConcurrentLookup() {
  TempDatabase temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "concurrent lookup database open")) return false;
  oursql::BufferPoolManager buffer_pool(16, &disk);
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 4, 4);
  for (std::int64_t key = 1; key <= 200; ++key) {
    if (!Check(tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1}).ok(),
               "concurrent lookup fixture insert")) return false;
  }

  constexpr std::size_t kReaders = 8;
  StartGate gate(kReaders);
  std::vector<std::uint8_t> succeeded(kReaders, 1);
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  for (std::size_t reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&, reader] {
      gate.Wait();
      for (std::int64_t key = 1; key <= 200; ++key) {
        auto found = tree.GetValue(key);
        if (!found.ok() || found.value().size() != 1 ||
            found.value()[0] !=
                oursql::RID{static_cast<oursql::page_id_t>(key), 1}) {
          succeeded[reader] = 0;
          return;
        }
      }
    });
  }
  for (auto &reader : readers) reader.join();
  for (const auto ok : succeeded) {
    if (!Check(ok, "concurrent lookup should preserve every RID")) return false;
  }
  return true;
}

bool TestLookupDuringInsertAndSplit() {
  TempDatabase temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "lookup/insert database open")) return false;
  oursql::BufferPoolManager buffer_pool(32, &disk);
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 4, 4);
  for (std::int64_t key = 1; key <= 200; ++key) {
    if (!Check(tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1}).ok(),
               "lookup/insert fixture insert")) return false;
  }

  constexpr std::size_t kReaders = 4;
  StartGate gate(kReaders + 1);
  std::vector<std::uint8_t> succeeded(kReaders, 1);
  std::vector<std::thread> readers;
  for (std::size_t reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&, reader] {
      gate.Wait();
      for (std::size_t pass = 0; pass < 10; ++pass) {
        for (std::int64_t key = 1; key <= 200; ++key) {
          auto found = tree.GetValue(key);
          if (!found.ok() || found.value().size() != 1) {
            succeeded[reader] = 0;
            return;
          }
        }
      }
    });
  }
  oursql::Status writer_status =
      oursql::Status::InternalError("lookup/insert writer unfinished");
  std::thread writer([&] {
    gate.Wait();
    for (std::int64_t key = 201; key <= 400; ++key) {
      writer_status = tree.Insert(
          key, oursql::RID{static_cast<oursql::page_id_t>(key), 1});
      if (!writer_status.ok()) return;
    }
  });
  writer.join();
  for (auto &reader : readers) reader.join();
  if (!Check(writer_status.ok(), "concurrent inserts and splits should finish")) return false;
  for (const auto ok : succeeded) {
    if (!Check(ok, "lookup should stay valid during insert/split")) return false;
  }
  auto all = tree.RangeScan(1, 400);
  return Check(all.ok() && all.value().size() == 400,
               "lookup/insert tree should retain an ordered complete leaf chain");
}

bool TestConcurrentUniqueKeyInsert() {
  using namespace std::chrono_literals;
  TempDatabase temp;
  oursql::DiskManager disk;
  oursql::LogManager log;
  oursql::LockManager locks;
  if (!Check(disk.Open(temp.path).ok(), "unique-key database open") ||
      !Check(log.Open(temp.path).ok(), "unique-key WAL open")) return false;
  oursql::BufferPoolManager buffer_pool(12, &disk);
  if (!Check(buffer_pool.SetLogManager(&log).ok(), "unique-key WAL binding") ||
      !Check(buffer_pool.SetLockManager(&locks).ok(), "unique-key lock binding")) {
    return false;
  }
  oursql::Catalog catalog(&buffer_pool, &disk);
  if (!Check(catalog.Open().ok(), "unique-key catalog open") ||
      !Check(catalog.CreateTable(
                 oursql::TableMetadata{"items", oursql::Schema({
                     oursql::Column{"id", oursql::DataType::Int}})})
                 .ok(),
             "unique-key table create")) return false;
  oursql::IndexManager indexes(&catalog, &buffer_pool, &locks);
  if (!Check(indexes.CreateIndex("idx_items_id", "items", "id", true).ok(),
             "unique-key index create")) return false;

  oursql::TransactionManager transactions(&log, &buffer_pool, &disk, &locks);
  auto first = transactions.Begin();
  auto second = transactions.Begin();
  if (!Check(first.ok() && second.ok(), "unique-key transactions begin")) return false;
  std::vector<oursql::Status> status{
      oursql::Status::InternalError("first insert unfinished"),
      oursql::Status::InternalError("second insert unfinished")};
  std::mutex state_mutex;
  std::condition_variable changed;
  std::size_t completed = 0;
  StartGate gate(2);
  std::thread first_worker([&] {
    gate.Wait();
    status[0] = indexes.InsertEntry("idx_items_id", 7, oursql::RID{11, 1},
                                    first.value().get());
    std::lock_guard<std::mutex> lock(state_mutex);
    ++completed;
    changed.notify_all();
  });
  std::thread second_worker([&] {
    gate.Wait();
    status[1] = indexes.InsertEntry("idx_items_id", 7, oursql::RID{12, 1},
                                    second.value().get());
    std::lock_guard<std::mutex> lock(state_mutex);
    ++completed;
    changed.notify_all();
  });
  bool first_completed = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    first_completed = changed.wait_for(lock, 2s, [&] { return completed == 1; });
  }
  if (!Check(first_completed,
             "one unique-key insert should finish while its peer waits")) {
    (void)transactions.Abort(first.value());
    (void)transactions.Abort(second.value());
    first_worker.join();
    second_worker.join();
    return false;
  }
  const std::size_t winner = status[0].ok() ? 0 : 1;
  const std::size_t loser = 1 - winner;
  auto winner_txn = winner == 0 ? first.value() : second.value();
  auto loser_txn = loser == 0 ? first.value() : second.value();
  if (!Check(status[winner].ok(), "one unique-key insert should win") ||
      !Check(transactions.Commit(winner_txn).ok(), "unique-key winner commit")) {
    (void)transactions.Abort(first.value());
    (void)transactions.Abort(second.value());
    first_worker.join();
    second_worker.join();
    return false;
  }
  bool loser_completed = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    loser_completed = changed.wait_for(lock, 2s, [&] { return completed == 2; });
  }
  if (!Check(loser_completed,
             "unique-key loser should resume after winner commit")) {
    (void)transactions.Abort(loser_txn);
    first_worker.join();
    second_worker.join();
    return false;
  }
  first_worker.join();
  second_worker.join();
  if (!Check(!status[loser].ok() &&
                 status[loser].code() == oursql::ErrorCode::AlreadyExists,
             "exactly one concurrent unique-key insert should fail") ||
      !Check(transactions.Abort(loser_txn).ok(), "unique-key loser rollback")) return false;
  auto found = indexes.Lookup("idx_items_id", 7);
  return Check(found.ok() && found.value().size() == 1,
               "unique index should contain one RID after commit/rollback") &&
         Check(buffer_pool.Close().ok() && log.Close().ok() && disk.Close().ok(),
               "unique-key resources close");
}

bool TestConcurrentDifferentLeafInsert() {
  using namespace std::chrono_literals;
  TempDatabase temp;
  oursql::DiskManager disk;
  oursql::LogManager log;
  oursql::LockManager locks;
  if (!Check(disk.Open(temp.path).ok(), "different-leaf database open") ||
      !Check(log.Open(temp.path).ok(), "different-leaf WAL open")) return false;
  oursql::BufferPoolManager buffer_pool(32, &disk);
  if (!Check(buffer_pool.SetLogManager(&log).ok(), "different-leaf WAL binding") ||
      !Check(buffer_pool.SetLockManager(&locks).ok(), "different-leaf lock binding")) {
    return false;
  }
  oursql::BPlusTree tree(&buffer_pool, oursql::INVALID_PAGE_ID, 8, 8);
  for (std::int64_t key = 2; key <= 400; key += 2) {
    if (!Check(tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key), 1}).ok(),
               "different-leaf fixture insert")) return false;
  }
  if (!Check(buffer_pool.FlushAllPages().ok(), "different-leaf fixture flush")) return false;
  oursql::TransactionManager transactions(&log, &buffer_pool, &disk, &locks);
  auto first = transactions.Begin();
  auto second = transactions.Begin();
  if (!Check(first.ok() && second.ok(), "different-leaf transactions begin")) return false;

  std::vector<oursql::Status> status{
      oursql::Status::InternalError("first leaf insert unfinished"),
      oursql::Status::InternalError("second leaf insert unfinished")};
  std::mutex state_mutex;
  std::condition_variable changed;
  std::size_t completed = 0;
  StartGate gate(2);
  std::thread first_worker([&] {
    gate.Wait();
    status[0] = tree.Insert(51, oursql::RID{501, 1}, first.value().get());
    std::lock_guard<std::mutex> lock(state_mutex);
    ++completed;
    changed.notify_all();
  });
  std::thread second_worker([&] {
    gate.Wait();
    status[1] = tree.Insert(351, oursql::RID{502, 1}, second.value().get());
    std::lock_guard<std::mutex> lock(state_mutex);
    ++completed;
    changed.notify_all();
  });
  // 在任何事务提交前等待两个写线程都返回；若树被一把全局写锁包住，
  // 第二个线程会一直等到提交，此断言便会确定性失败。
  bool both_completed = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    both_completed = changed.wait_for(lock, 2s, [&] { return completed == 2; });
  }
  if (!Check(both_completed,
             "different leaf inserts should both finish before either commit")) {
    (void)transactions.ResolveDeadlocksOnce();
    (void)transactions.Abort(first.value());
    (void)transactions.Abort(second.value());
    first_worker.join();
    second_worker.join();
    return false;
  }
  first_worker.join();
  second_worker.join();
  if (!Check(status[0].ok() && status[1].ok(),
             "different leaf inserts should both succeed") ||
      !Check(transactions.Commit(first.value()).ok() &&
                 transactions.Commit(second.value()).ok(),
             "different leaf inserts commit")) return false;
  auto first_found = tree.GetValue(51);
  auto second_found = tree.GetValue(351);
  return Check(first_found.ok() && first_found.value().size() == 1 &&
                   second_found.ok() && second_found.value().size() == 1,
               "different leaf inserts should remain searchable");
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
  run("empty tree", &TestEmptyTree);
  run("splits lookup and leaf chain", &TestSplitsLookupAndLeafChain);
  run("duplicate keys and reopen", &TestDuplicateKeysAndReopen);
  run("delete rebalance and root contraction", &TestDeleteRebalanceAndRootContraction);
  run("delete reclaims pages across restart", &TestDeleteReclaimsPagesAcrossRestart);
  run("transactional structural changes", &TestTransactionalStructuralChanges);
  run("transactional write release failure",
      &TestTransactionalWriteReleaseFailurePropagates);
  run("concurrent lookup", &TestConcurrentLookup);
  run("lookup during insert and split", &TestLookupDuringInsertAndSplit);
  run("concurrent unique key insert", &TestConcurrentUniqueKeyInsert);
  run("concurrent different leaf insert", &TestConcurrentDifferentLeafInsert);
  return failures == 0 ? 0 : 1;
}
