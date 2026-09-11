#include "oursql/index/b_plus_tree.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
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
  return failures == 0 ? 0 : 1;
}
