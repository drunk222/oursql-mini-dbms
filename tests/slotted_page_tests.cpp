#include "oursql/storage/storage.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_slotted_page_" + std::to_string(stamp) + ".oursql");
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

std::vector<std::byte> Bytes(const std::string &text) {
  std::vector<std::byte> result;
  result.reserve(text.size());
  for (const auto character : text) {
    result.push_back(std::byte{static_cast<unsigned char>(character)});
  }
  return result;
}

std::vector<std::byte> Filled(std::size_t size, unsigned char value) {
  return std::vector<std::byte>(size, std::byte{value});
}

bool SameBytes(const std::vector<std::byte> &lhs, const std::vector<std::byte> &rhs) {
  return lhs == rhs;
}

bool OpenDb(oursql::DiskManager &disk_manager, const std::filesystem::path &path) {
  return Check(disk_manager.Open(path).ok(), "打开数据库文件应成功");
}

bool CloseDb(oursql::DiskManager &disk_manager) {
  return Check(disk_manager.Close().ok(), "关闭数据库文件应成功");
}

bool TestVariableLengthRecordsAndDelete() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!OpenDb(disk_manager, temp.path)) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(1, &disk_manager);
  auto new_page = buffer_pool.NewPage();
  if (!Check(new_page.ok(), "申请 Slotted Page 应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  const auto page_id = new_page.value().PageId();
  std::vector<oursql::slot_id_t> slots;
  const std::vector<std::vector<std::byte>> records{
      Bytes("a"), Bytes("variable length record"), Bytes("最后一条")};
  {
    auto page = std::move(new_page.value());
    if (!Check(oursql::SlottedPage::Initialize(page).ok(), "页面初始化应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    for (const auto &record : records) {
      auto slot = oursql::SlottedPage::InsertRecord(page, record);
      if (!Check(slot.ok(), "变长记录插入应成功")) {
        CloseDb(disk_manager);
        return false;
      }
      slots.push_back(slot.value());
    }
  }
  {
    auto read_result = buffer_pool.FetchPage(page_id);
    if (!Check(read_result.ok() && oursql::SlottedPage::Validate(read_result.value()).ok(),
               "页面校验应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    auto read_page = std::move(read_result.value());
    for (std::size_t i = 0; i < records.size(); ++i) {
      auto record = oursql::SlottedPage::GetRecord(read_page, slots[i]);
      if (!Check(record.ok() && SameBytes(record.value(), records[i]), "记录应逐条读回")) {
        CloseDb(disk_manager);
        return false;
      }
    }
  }
  {
    auto write_result = buffer_pool.FetchPageWrite(page_id);
    if (!Check(write_result.ok(), "删除页面 Fetch 应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    auto write_page = std::move(write_result.value());
    if (!Check(oursql::SlottedPage::DeleteRecord(write_page, slots[1]).ok(), "删除记录应成功")) {
      CloseDb(disk_manager);
      return false;
    }
  }
  auto read_result = buffer_pool.FetchPage(page_id);
  if (!Check(read_result.ok(), "删除后读取页面应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  auto read_page = std::move(read_result.value());
  auto deleted = oursql::SlottedPage::GetRecord(read_page, slots[1]);
  auto remaining = oursql::SlottedPage::GetRecord(read_page, slots[2]);
  const bool ok = Check(!deleted.ok() && deleted.status().code() == oursql::ErrorCode::NotFound,
                        "已删除 slot 不得返回旧数据") &&
                 Check(remaining.ok() && SameBytes(remaining.value(), records[2]),
                       "其他 slot 内容不应受删除影响");
  return CloseDb(disk_manager) && ok;
}

bool TestOutOfSpaceAndAutomaticCompaction() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!OpenDb(disk_manager, temp.path)) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(1, &disk_manager);
  auto new_page = buffer_pool.NewPage();
  if (!Check(new_page.ok(), "申请页面应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  const auto page_id = new_page.value().PageId();
  {
    auto page = std::move(new_page.value());
    if (!Check(oursql::SlottedPage::Initialize(page).ok(), "页面初始化应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    auto too_large = oursql::SlottedPage::InsertRecord(page, Filled(4060, 0x11));
    if (!Check(!too_large.ok() && too_large.status().code() == oursql::ErrorCode::OutOfSpace,
               "超大记录应返回 OutOfSpace")) {
      CloseDb(disk_manager);
      return false;
    }
    const auto first = Filled(900, 0x01);
    const auto second = Filled(900, 0x02);
    const auto third = Filled(900, 0x03);
    auto slot0 = oursql::SlottedPage::InsertRecord(page, first);
    auto slot1 = oursql::SlottedPage::InsertRecord(page, second);
    auto slot2 = oursql::SlottedPage::InsertRecord(page, third);
    if (!Check(slot0.ok() && slot1.ok() && slot2.ok(), "碎片测试记录插入应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    if (!Check(oursql::SlottedPage::DeleteRecord(page, slot1.value()).ok(), "制造碎片应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    auto slot3 = oursql::SlottedPage::InsertRecord(page, Filled(1400, 0x44));
    if (!Check(slot3.ok() && slot3.value() == 3, "连续空间不足但碎片足够时应自动整理后插入")) {
      CloseDb(disk_manager);
      return false;
    }
  }
  auto read_result = buffer_pool.FetchPage(page_id);
  if (!Check(read_result.ok(), "整理后页面读取应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  auto read_page = std::move(read_result.value());
  auto slot0 = oursql::SlottedPage::GetRecord(read_page, 0);
  auto slot1 = oursql::SlottedPage::GetRecord(read_page, 1);
  auto slot2 = oursql::SlottedPage::GetRecord(read_page, 2);
  auto slot3 = oursql::SlottedPage::GetRecord(read_page, 3);
  const bool ok = Check(slot0.ok() && slot0.value() == Filled(900, 0x01), "整理不得改变 slot 0") &&
                  Check(!slot1.ok(), "删除的 slot 1 仍应无效") &&
                  Check(slot2.ok() && slot2.value() == Filled(900, 0x03), "整理不得改变 slot 2") &&
                  Check(slot3.ok() && slot3.value() == Filled(1400, 0x44), "新 slot 内容应正确");
  return CloseDb(disk_manager) && ok;
}

bool TestExplicitCompactAndRestart() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!OpenDb(disk_manager, temp.path)) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(1, &disk_manager);
  auto new_page = buffer_pool.NewPage();
  if (!Check(new_page.ok(), "申请页面应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  const auto page_id = new_page.value().PageId();
  const auto first = Bytes("first");
  const auto second = Bytes("second record");
  {
    auto page = std::move(new_page.value());
    if (!Check(oursql::SlottedPage::Initialize(page, oursql::INVALID_PAGE_ID).ok(), "页面初始化应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    auto slot0 = oursql::SlottedPage::InsertRecord(page, first);
    auto slot1 = oursql::SlottedPage::InsertRecord(page, second);
    if (!Check(slot0.ok() && slot1.ok(), "记录插入应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    if (!Check(oursql::SlottedPage::DeleteRecord(page, slot0.value()).ok(), "删除应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    if (!Check(oursql::SlottedPage::Compact(page).ok(), "显式整理应成功")) {
      CloseDb(disk_manager);
      return false;
    }
  }
  if (!Check(buffer_pool.Close().ok(), "缓冲池关闭应刷盘成功") || !CloseDb(disk_manager)) {
    return false;
  }

  oursql::DiskManager reopened_disk;
  if (!OpenDb(reopened_disk, temp.path)) {
    return false;
  }
  oursql::BufferPoolManager reopened_pool(1, &reopened_disk);
  auto read_result = reopened_pool.FetchPage(page_id);
  if (!Check(read_result.ok(), "重启后页面读取应成功")) {
    CloseDb(reopened_disk);
    return false;
  }
  auto read_page = std::move(read_result.value());
  auto deleted = oursql::SlottedPage::GetRecord(read_page, 0);
  auto preserved = oursql::SlottedPage::GetRecord(read_page, 1);
  const bool ok = Check(!deleted.ok(), "重启后删除 slot 仍应无效") &&
                  Check(preserved.ok() && SameBytes(preserved.value(), second),
                        "重启后有效记录应保持内容");
  return CloseDb(reopened_disk) && ok;
}

bool TestCorruptMetadataIsRejected() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!OpenDb(disk_manager, temp.path)) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(1, &disk_manager);
  auto new_page = buffer_pool.NewPage();
  if (!Check(new_page.ok(), "申请页面应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  const auto page_id = new_page.value().PageId();
  {
    auto page = std::move(new_page.value());
    if (!Check(oursql::SlottedPage::Initialize(page).ok(), "页面初始化应成功")) {
      CloseDb(disk_manager);
      return false;
    }
    page.Data()[oursql::SlottedPage::kFreeStartOffset] = std::byte{0x1F};
    page.Data()[oursql::SlottedPage::kFreeStartOffset + 1] = std::byte{0};
    page.Data()[oursql::SlottedPage::kFreeStartOffset + 2] = std::byte{0};
    page.Data()[oursql::SlottedPage::kFreeStartOffset + 3] = std::byte{0};
    if (!Check(page.MarkDirty().ok(), "损坏页面标脏应成功")) {
      CloseDb(disk_manager);
      return false;
    }
  }
  if (!Check(buffer_pool.FlushPage(page_id).ok(), "损坏页面刷盘应成功")) {
    CloseDb(disk_manager);
    return false;
  }
  auto read_result = buffer_pool.FetchPage(page_id);
  if (!Check(read_result.ok(), "损坏页面仍应可以安全读取原始字节")) {
    CloseDb(disk_manager);
    return false;
  }
  auto read_page = std::move(read_result.value());
  const auto status = oursql::SlottedPage::Validate(read_page);
  const bool ok = Check(!status.ok() && status.code() == oursql::ErrorCode::InvalidArgument,
                        "非法页面元数据应返回明确错误");
  return CloseDb(disk_manager) && ok;
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

  run("Variable length records and delete", &TestVariableLengthRecordsAndDelete);
  run("Out of space and automatic compaction", &TestOutOfSpaceAndAutomaticCompaction);
  run("Explicit compact and restart", &TestExplicitCompactAndRestart);
  run("Corrupt metadata rejection", &TestCorruptMetadataIsRejected);

  if (failures == 0) {
    std::cout << "All slotted page tests passed\n";
    return 0;
  }
  std::cerr << failures << " slotted page test(s) failed\n";
  return 1;
}
