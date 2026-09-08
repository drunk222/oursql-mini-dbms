#include "oursql/catalog/catalog.h"
#include "oursql/storage/heap_table.h"
#include "oursql/storage/row_codec.h"

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
    path = std::filesystem::temp_directory_path() / ("oursql_catalog_heap_" + std::to_string(stamp) + ".oursql");
  }

  ~TempDb() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool SameValue(const oursql::Value &lhs, const oursql::Value &rhs) {
  return lhs == rhs;
}

bool SameRow(const oursql::Row &lhs, const oursql::Row &rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (!SameValue(lhs[i], rhs[i])) return false;
  }
  return true;
}

oursql::Schema UserSchema() {
  return oursql::Schema{{"id", oursql::DataType::Int}, {"name", oursql::DataType::Varchar, 64}};
}

bool TestRowCodec() {
  const auto schema = UserSchema();
  const oursql::Row row{oursql::Value(static_cast<std::int64_t>(42)), oursql::Value("Alice")};
  auto encoded = oursql::RowCodec::Encode(schema, row);
  if (!Check(encoded.ok(), "INT/VARCHAR 行编码应成功")) return false;
  auto decoded = oursql::RowCodec::Decode(schema, encoded.value());
  if (!Check(decoded.ok() && SameRow(decoded.value(), row), "INT/VARCHAR 行往返应一致")) return false;

  auto wrong_count = oursql::RowCodec::Encode(schema, {oursql::Value(static_cast<std::int64_t>(1))});
  if (!Check(!wrong_count.ok(), "Row 列数错误应被拒绝")) return false;
  auto wrong_type = oursql::RowCodec::Encode(
      schema, {oursql::Value("not int"), oursql::Value("Alice")});
  if (!Check(!wrong_type.ok() && wrong_type.status().code() == oursql::ErrorCode::TypeMismatch,
             "Row 类型错误应被拒绝")) return false;

  auto truncated = encoded.value();
  truncated.pop_back();
  auto corrupt = oursql::RowCodec::Decode(schema, truncated);
  if (!Check(!corrupt.ok(), "损坏 Row 记录应被拒绝")) return false;
  auto extra = encoded.value();
  extra.push_back(std::byte{0});
  auto trailing = oursql::RowCodec::Decode(schema, extra);
  return Check(!trailing.ok(), "Row 多余数据应被拒绝");
}

bool TestCatalogPersistenceAndChain() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!Check(disk_manager.Open(temp.path).ok(), "Catalog 数据库打开应成功")) return false;
  oursql::BufferPoolManager buffer_pool(4, &disk_manager);
  oursql::Catalog catalog(&buffer_pool, &disk_manager);
  if (!Check(catalog.Open().ok(), "首次打开 Catalog 应创建入口页")) return false;

  const auto schema = UserSchema();
  for (int i = 0; i < 110; ++i) {
    auto status = catalog.CreateTable(
        oursql::TableMetadata{"table" + std::to_string(i), schema, oursql::INVALID_PAGE_ID});
    if (!Check(status.ok(), "Catalog 建表应成功")) return false;
  }
  if (!Check(catalog.ListTables().size() == 110, "Catalog 应列出全部表")) return false;
  auto found = catalog.GetTableMetadata("table109");
  if (!Check(found.ok() && found.value()->first_data_page_id != oursql::INVALID_PAGE_ID,
             "表元数据应包含首数据页")) return false;
  if (!Check(buffer_pool.Close().ok() && disk_manager.Close().ok(), "Catalog 关闭应成功")) return false;

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "重启后数据库打开应成功")) return false;
  oursql::BufferPoolManager reopened_pool(4, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "重启后 Catalog 加载应成功")) return false;
  auto reopened_schema = reopened_catalog.GetSchema("table109");
  const bool ok = Check(reopened_catalog.ListTables().size() == 110, "重启后 Catalog 表数量应一致") &&
                  Check(reopened_schema.ok() && reopened_schema.value()->size() == 2,
                        "重启后 Schema 应存在") &&
                  Check(reopened_schema.value()->At(1).type == oursql::DataType::Varchar,
                        "重启后列类型应一致");
  return reopened_pool.Close().ok() && reopened_disk.Close().ok() && ok;
}

bool TestHeapTableAcrossPagesDeleteAndRestart() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!Check(disk_manager.Open(temp.path).ok(), "HeapTable 数据库打开应成功")) return false;
  oursql::BufferPoolManager buffer_pool(3, &disk_manager);
  oursql::Catalog catalog(&buffer_pool, &disk_manager);
  if (!Check(catalog.Open().ok(), "HeapTable Catalog 打开应成功")) return false;
  const auto schema = UserSchema();
  if (!Check(catalog.CreateTable(oursql::TableMetadata{"users", schema, oursql::INVALID_PAGE_ID}).ok(),
             "users 建表应成功")) return false;
  auto metadata_result = catalog.GetTableMetadata("users");
  if (!Check(metadata_result.ok(), "应能取得 users 元数据")) return false;
  oursql::HeapTable table(&buffer_pool, *metadata_result.value());

  std::vector<oursql::RID> rids;
  std::vector<oursql::Row> expected;
  for (std::int64_t i = 0; i < 120; ++i) {
    const auto row = oursql::Row{oursql::Value(i), oursql::Value("user_" + std::to_string(i))};
    auto rid = table.InsertRow(row);
    if (!Check(rid.ok(), "跨页插入应成功")) return false;
    rids.push_back(rid.value());
    expected.push_back(row);
  }
  auto scanned = table.Scan();
  if (!Check(scanned.ok() && scanned.value().size() == expected.size(), "跨页扫描数量应正确")) return false;
  for (std::size_t i = 0; i < scanned.value().size(); ++i) {
    if (!Check(SameRow(scanned.value()[i].second, expected[i]), "跨页扫描内容应按顺序一致")) return false;
  }

  for (std::size_t i = 0; i < rids.size(); i += 4) {
    if (!Check(table.DeleteRow(rids[i]).ok(), "删除指定行应成功")) return false;
  }
  for (std::size_t i = 0; i < rids.size(); i += 4) {
    if (!Check(!table.GetRow(rids[i]).ok(), "删除后的 RID 不应读取成功")) return false;
  }
  scanned = table.Scan();
  if (!Check(scanned.ok() && scanned.value().size() == expected.size() - 30,
             "删除后扫描应跳过无效行")) return false;
  for (const auto &entry : scanned.value()) {
    if (!Check(entry.second[0].AsInt() % 4 != 0, "扫描不应包含已删除行")) return false;
  }
  if (!Check(buffer_pool.Close().ok() && disk_manager.Close().ok(), "HeapTable 关闭应成功")) return false;

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "HeapTable 重启打开应成功")) return false;
  oursql::BufferPoolManager reopened_pool(3, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "HeapTable 重启 Catalog 应成功")) return false;
  auto metadata = reopened_catalog.GetTableMetadata("users");
  if (!Check(metadata.ok(), "重启后 users 元数据应存在")) return false;
  oursql::HeapTable reopened_table(&reopened_pool, *metadata.value());
  auto reopened_scan = reopened_table.Scan();
  const bool ok = Check(reopened_scan.ok() && reopened_scan.value().size() == expected.size() - 30,
                        "重启后跨页扫描数量应一致");
  return reopened_pool.Close().ok() && reopened_disk.Close().ok() && ok;
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) std::cout << "[PASS] " << name << '\n';
    else ++failures;
  };
  run("Row codec", &TestRowCodec);
  run("Catalog persistence and chain", &TestCatalogPersistenceAndChain);
  run("HeapTable pages delete and restart", &TestHeapTableAcrossPagesDeleteAndRestart);
  if (failures == 0) {
    std::cout << "All catalog and heap tests passed\n";
    return 0;
  }
  std::cerr << failures << " catalog/heap test(s) failed\n";
  return 1;
}
