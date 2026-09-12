#include "oursql/catalog/catalog.h"
#include "oursql/index/b_plus_tree.h"
#include "oursql/index/index_manager.h"
#include "oursql/storage/heap_table.h"
#include "oursql/storage/row_codec.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
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

bool TestCatalogHeadIsDurableImmediately() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!Check(disk_manager.Open(temp.path).ok(), "Catalog 即时持久化测试数据库打开应成功")) {
    return false;
  }
  oursql::BufferPoolManager buffer_pool(2, &disk_manager);
  oursql::Catalog catalog(&buffer_pool, &disk_manager);
  if (!Check(catalog.Open().ok(), "首次打开 Catalog 应成功")) return false;

  // Page 0 is the superblock, so a fresh database allocates Page 1 as its Catalog head.
  auto persisted_page = disk_manager.ReadPage(1);
  if (!Check(persisted_page.ok(), "Catalog 首页应能直接从磁盘读取")) return false;
  const auto page_type = static_cast<std::uint32_t>(persisted_page.value().Data()[0]) |
                         (static_cast<std::uint32_t>(persisted_page.value().Data()[1]) << 8U) |
                         (static_cast<std::uint32_t>(persisted_page.value().Data()[2]) << 16U) |
                         (static_cast<std::uint32_t>(persisted_page.value().Data()[3]) << 24U);
  const bool ok = Check(page_type == oursql::SlottedPage::kPageType,
                        "superblock 发布 Catalog 首页前，该页必须已经持久化");
  return buffer_pool.Close().ok() && disk_manager.Close().ok() && ok;
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
  if (!Check(scanned.ok() && scanned.value().size() == expected.size(), "跨页扫描数量应正确")) {
    if (!scanned.ok()) std::cerr << scanned.status().ToString() << '\n';
    else std::cerr << "实际扫描数量: " << scanned.value().size() << '\n';
    return false;
  }
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

bool TestRidOwnership() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!Check(disk_manager.Open(temp.path).ok(), "RID 归属测试数据库打开应成功")) return false;
  oursql::BufferPoolManager buffer_pool(3, &disk_manager);
  oursql::Catalog catalog(&buffer_pool, &disk_manager);
  if (!Check(catalog.Open().ok(), "RID 归属测试 Catalog 打开应成功")) return false;
  const auto schema = UserSchema();
  if (!Check(catalog.CreateTable(oursql::TableMetadata{"left_table", schema, oursql::INVALID_PAGE_ID}).ok() &&
                 catalog.CreateTable(oursql::TableMetadata{"right_table", schema, oursql::INVALID_PAGE_ID}).ok(),
             "RID 归属测试建表应成功")) {
    return false;
  }
  auto left_metadata = catalog.GetTableMetadata("left_table");
  auto right_metadata = catalog.GetTableMetadata("right_table");
  if (!Check(left_metadata.ok() && right_metadata.ok(), "RID 归属测试应取得两张表元数据")) return false;
  oursql::HeapTable left(&buffer_pool, *left_metadata.value());
  oursql::HeapTable right(&buffer_pool, *right_metadata.value());
  auto right_rid = right.InsertRow({oursql::Value(static_cast<std::int64_t>(7)), oursql::Value("right")});
  if (!Check(right_rid.ok(), "RID 归属测试插入应成功")) return false;
  auto foreign_row = left.GetRow(right_rid.value());
  if (!Check(!foreign_row.ok() && foreign_row.status().code() == oursql::ErrorCode::NotFound,
             "其他表 RID 不应被当前表读取")) {
    return false;
  }
  if (!Check(left.DeleteRow(right_rid.value()).code() == oursql::ErrorCode::NotFound,
             "其他表 RID 不应被当前表删除")) {
    return false;
  }
  auto preserved = right.GetRow(right_rid.value());
  return Check(preserved.ok(), "拒绝跨表 RID 后原表数据仍应存在") &&
         buffer_pool.Close().ok() && disk_manager.Close().ok();
}

bool TestOversizedRecordsDoNotAllocatePages() {
  TempDb temp;
  oursql::DiskManager disk_manager;
  if (!Check(disk_manager.Open(temp.path).ok(), "超大记录测试数据库打开应成功")) return false;
  oursql::BufferPoolManager buffer_pool(3, &disk_manager);
  oursql::Catalog catalog(&buffer_pool, &disk_manager);
  if (!Check(catalog.Open().ok(), "超大记录测试 Catalog 打开应成功")) return false;

  const auto schema = oursql::Schema{{"id", oursql::DataType::Int}, {"payload", oursql::DataType::Varchar}};
  const auto catalog_size_before = std::filesystem::file_size(temp.path);
  const auto huge_name = std::string(5000, 't');
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto status = catalog.CreateTable(oursql::TableMetadata{huge_name, schema, oursql::INVALID_PAGE_ID});
    if (!Check(status.code() == oursql::ErrorCode::RecordTooLarge,
               "超大 Catalog 元数据应返回 RecordTooLarge")) return false;
    if (!Check(std::filesystem::file_size(temp.path) == catalog_size_before,
               "超大 Catalog 元数据失败不得增加页面")) return false;
  }

  if (!Check(catalog.CreateTable(oursql::TableMetadata{"ordinary", schema, oursql::INVALID_PAGE_ID}).ok(),
             "超大元数据失败后小表仍应可创建")) return false;
  auto metadata = catalog.GetTableMetadata("ordinary");
  if (!Check(metadata.ok(), "超大元数据失败后应能取得普通表元数据")) return false;
  oursql::HeapTable table(&buffer_pool, *metadata.value());
  const auto row_size_before = std::filesystem::file_size(temp.path);
  const oursql::Row huge_row{oursql::Value(static_cast<std::int64_t>(1)),
                             oursql::Value(std::string(4040, 'x'))};
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto rid = table.InsertRow(huge_row);
    if (!Check(!rid.ok() && rid.status().code() == oursql::ErrorCode::RecordTooLarge,
               "超大 Row 应返回 RecordTooLarge")) return false;
    if (!Check(std::filesystem::file_size(temp.path) == row_size_before,
               "超大 Row 失败不得申请新页面")) return false;
  }
  auto ordinary_row = table.InsertRow({oursql::Value(static_cast<std::int64_t>(7)), oursql::Value("ok")});
  if (!Check(ordinary_row.ok(), "超大 Row 失败后普通 Row 仍应插入")) return false;
  if (!Check(buffer_pool.Close().ok() && disk_manager.Close().ok(), "超大记录测试关闭应成功")) return false;

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "超大记录测试重启打开应成功")) return false;
  oursql::BufferPoolManager reopened_pool(3, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "超大记录测试重启 Catalog 应成功")) return false;
  auto reopened_metadata = reopened_catalog.GetTableMetadata("ordinary");
  if (!Check(reopened_metadata.ok(), "超大记录测试重启表元数据应存在")) return false;
  oursql::HeapTable reopened_table(&reopened_pool, *reopened_metadata.value());
  auto scanned = reopened_table.Scan();
  const bool ok = Check(scanned.ok() && scanned.value().size() == 1,
                        "超大记录失败后重启扫描应只有普通 Row") &&
                  Check(scanned.value()[0].second[0].AsInt() == 7 &&
                            scanned.value()[0].second[1].AsVarchar() == "ok",
                        "超大记录失败后普通 Row 内容应恢复");
  return reopened_pool.Close().ok() && reopened_disk.Close().ok() && ok;
}

bool TestIndexMetadataPersistence() {
  TempDb temp;
  oursql::page_id_t expected_root = oursql::INVALID_PAGE_ID;
  {
    oursql::DiskManager disk_manager;
    if (!Check(disk_manager.Open(temp.path).ok(), "索引元数据数据库打开应成功")) return false;
    oursql::BufferPoolManager buffer_pool(6, &disk_manager);
    oursql::Catalog catalog(&buffer_pool, &disk_manager);
    if (!Check(catalog.Open().ok(), "索引元数据Catalog打开应成功")) return false;
    const auto schema = UserSchema();
    if (!Check(catalog.CreateTable(
                   oursql::TableMetadata{"student", schema, oursql::INVALID_PAGE_ID}).ok(),
               "student建表应成功")) return false;

    auto tree_result = oursql::BPlusTree::CreatePersistent(&buffer_pool, 3, 3);
    if (!Check(tree_result.ok(), "索引B+树创建应成功")) return false;
    auto &tree = *tree_result.value();
    if (!Check(tree.Insert(1, oursql::RID{2, 1}).ok(), "索引首条记录插入应成功")) return false;
    if (!Check(catalog.CreateIndex(oursql::IndexMetadata{
                   "idx_student_id", "student", "id", tree.GetRootPageId(), true}).ok(),
               "索引元数据创建应成功")) return false;
    if (!Check(!catalog.CreateIndex(oursql::IndexMetadata{
                    "idx_student_id", "student", "id", tree.GetRootPageId(), true}).ok(),
               "重复索引名应被拒绝")) return false;
    if (!Check(!catalog.CreateIndex(oursql::IndexMetadata{
                    "idx_missing", "student", "missing", tree.GetRootPageId(), false}).ok(),
               "不存在的索引列应被拒绝")) return false;

    for (std::int64_t key = 2; key <= 30; ++key) {
      if (!Check(tree.Insert(key, oursql::RID{static_cast<oursql::page_id_t>(key + 1), 1}).ok(),
                 "索引扩展插入应成功")) return false;
    }
    expected_root = tree.GetRootPageId();
    if (!Check(catalog.UpdateIndexRootPageId("idx_student_id", expected_root).ok(),
               "根分裂后Catalog应更新root page id")) return false;
    if (!Check(catalog.ListIndexes().size() == 1 &&
                   catalog.ListTableIndexes("student").size() == 1,
               "Catalog应能列出全部索引和表索引")) return false;
    if (!Check(buffer_pool.Close().ok() && disk_manager.Close().ok(),
               "索引元数据数据库关闭应成功")) return false;
  }

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "索引元数据数据库重开应成功")) return false;
  oursql::BufferPoolManager reopened_pool(6, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "重开后索引元数据应恢复")) return false;
  auto index = reopened_catalog.FindIndex("idx_student_id");
  if (!Check(index.ok(), "重开后应按索引名查到元数据")) return false;
  if (!Check(index.value()->table_name == "student" && index.value()->column_name == "id" &&
                 index.value()->root_page_id == expected_root && index.value()->is_unique,
             "索引名、表、列、根Page ID和唯一性应完整恢复")) return false;
  oursql::BPlusTree reopened_tree(&reopened_pool, index.value()->root_page_id, 3, 3);
  auto found = reopened_tree.GetValue(30);
  const bool ok = Check(found.ok() && found.value().size() == 1,
                        "使用Catalog恢复的根Page ID应能查询B+树");
  return reopened_pool.Close().ok() && reopened_disk.Close().ok() && ok;
}

bool TestIndexManagerBuildMaintainAndDrop() {
  TempDb temp;
  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "IndexManager数据库打开应成功")) return false;
    oursql::BufferPoolManager pool(3, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok(), "IndexManager Catalog打开应成功")) return false;
    if (!Check(catalog.CreateTable(oursql::TableMetadata{
                   "student", UserSchema(), oursql::INVALID_PAGE_ID}).ok(),
               "IndexManager student建表应成功")) return false;
    auto table_metadata = catalog.GetTableMetadata("student");
    if (!table_metadata.ok()) return false;
    oursql::HeapTable table(&pool, *table_metadata.value());
    std::vector<oursql::RID> rids(1000);
    for (std::int64_t i = 0; i < 1000; ++i) {
      const auto key = (i * 37) % 1000;
      auto rid = table.InsertRow(
          {oursql::Value(key), oursql::Value("student_" + std::to_string(key))});
      if (!Check(rid.ok(), "1000个乱序Key写入堆表应成功")) return false;
      rids[static_cast<std::size_t>(key)] = rid.value();
    }

    oursql::IndexManager indexes(&catalog, &pool);
    if (!Check(indexes.CreateIndex("idx_student_id", "student", "id", true).ok(),
               "IndexManager应扫描现有1000行创建唯一索引")) return false;
    for (const auto key : {0LL, 127LL, 511LL, 999LL}) {
      auto found = indexes.Lookup("idx_student_id", key);
      if (!Check(found.ok() && found.value().size() == 1 &&
                     found.value()[0] == rids[static_cast<std::size_t>(key)],
                 "IndexManager等值查询应返回现有RID")) return false;
    }

    const oursql::Row inserted_row{oursql::Value(static_cast<std::int64_t>(1001)),
                                   oursql::Value("new")};
    auto inserted_rid = table.InsertRow(inserted_row);
    if (!Check(inserted_rid.ok() &&
                   indexes.OnInsert("student", inserted_row, inserted_rid.value()).ok(),
               "INSERT维护接口应增加索引项")) return false;

    const oursql::Row deleted_row{oursql::Value(static_cast<std::int64_t>(100)),
                                  oursql::Value("student_100")};
    if (!Check(indexes.OnDelete("student", deleted_row, rids[100]).ok() &&
                   table.DeleteRow(rids[100]).ok(),
               "DELETE维护接口应删除索引项")) return false;

    const oursql::Row old_row{oursql::Value(static_cast<std::int64_t>(200)),
                              oursql::Value("student_200")};
    const oursql::Row new_row{oursql::Value(static_cast<std::int64_t>(1200)),
                              oursql::Value("updated")};
    if (!Check(table.DeleteRow(rids[200]).ok(), "UPDATE模拟删除旧行应成功")) return false;
    auto new_rid = table.InsertRow(new_row);
    if (!Check(new_rid.ok() &&
                   indexes.OnUpdate("student", old_row, rids[200], new_row, new_rid.value()).ok(),
               "UPDATE维护接口应替换Key和RID")) return false;
    auto old_lookup = indexes.Lookup("idx_student_id", 200);
    auto new_lookup = indexes.Lookup("idx_student_id", 1200);
    if (!Check(old_lookup.ok() && old_lookup.value().empty() && new_lookup.ok() &&
                   new_lookup.value().size() == 1 && new_lookup.value()[0] == new_rid.value(),
               "UPDATE维护后只应命中新Key/RID")) return false;
    if (!Check(indexes.OnInsert(
                   "student",
                   {oursql::Value(static_cast<std::int64_t>(1)), oursql::Value("dup")},
                   oursql::RID{9999, 1}).code() == oursql::ErrorCode::AlreadyExists,
               "唯一索引维护应拒绝重复Key")) return false;
    if (!Check(pool.Close().ok() && disk.Close().ok(), "IndexManager首次关闭应成功")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "IndexManager重开数据库应成功")) return false;
    oursql::BufferPoolManager pool(3, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok(), "IndexManager重开Catalog应成功")) return false;
    oursql::IndexManager indexes(&catalog, &pool);
    auto restored = indexes.Lookup("idx_student_id", 1200);
    if (!Check(restored.ok() && restored.value().size() == 1,
               "重启后维护过的索引仍应可查询")) return false;
    if (!Check(indexes.DropIndex("idx_student_id").ok() && !catalog.HasIndex("idx_student_id"),
               "DROP INDEX应删除元数据并释放B+树页面")) return false;
    if (!Check(!indexes.Lookup("idx_student_id", 1200).ok(),
               "DROP INDEX后查询应返回NotFound")) return false;
    if (!Check(pool.Close().ok() && disk.Close().ok(), "IndexManager第二次关闭应成功")) return false;
  }
  {
    oursql::DiskManager disk;
    if (!disk.Open(temp.path).ok()) return false;
    oursql::BufferPoolManager pool(3, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok() && !catalog.HasIndex("idx_student_id"),
               "重启后已DROP的索引元数据不应恢复")) return false;
  }
  return true;
}

bool TestHeapTableDestroySinglePageAndPinnedRetry() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "Destroy test database should open")) return false;
  oursql::BufferPoolManager pool(3, &disk);
  oursql::page_id_t first_page = oursql::INVALID_PAGE_ID;
  {
    auto created = pool.NewPageGuarded();
    if (!Check(created.ok(), "Destroy test data page should allocate")) return false;
    auto page = std::move(created.value());
    first_page = page.PageId();
    if (!Check(oursql::SlottedPage::Initialize(page).ok(),
               "Destroy test data page should initialize")) return false;
  }
  oursql::HeapTable table(
      &pool, oursql::TableMetadata{"orphan_single",
                                   oursql::Schema{{"value", oursql::DataType::Int}},
                                   first_page});
  {
    auto external = pool.FetchPage(first_page);
    if (!Check(external.ok(), "External guard should pin the table page")) return false;
    const auto rejected = table.Destroy();
    if (!Check(rejected.code() == oursql::ErrorCode::InvalidArgument &&
                   rejected.message().find("被 pin 的页面不能删除") != std::string::npos,
               "Destroy should propagate the pinned-page contract")) return false;
  }
  if (!Check(table.Destroy().ok(), "Destroy should succeed after the external guard releases")) {
    return false;
  }
  {
    auto reused = pool.NewPage();
    if (!Check(reused.ok() && reused.value().PageId() == first_page,
               "Single-page Destroy should make the page immediately reusable")) return false;
  }
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Single-page Destroy test should close cleanly");
}

bool TestHeapTableDestroyMultiplePages() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "Multi-page Destroy database should open")) return false;
  oursql::BufferPoolManager pool(4, &disk);
  oursql::page_id_t first_page = oursql::INVALID_PAGE_ID;
  {
    auto created = pool.NewPageGuarded();
    if (!Check(created.ok(), "Multi-page first data page should allocate")) return false;
    auto page = std::move(created.value());
    first_page = page.PageId();
    if (!Check(oursql::SlottedPage::Initialize(page).ok(),
               "Multi-page first data page should initialize")) return false;
  }
  const oursql::Schema schema{{"id", oursql::DataType::Int},
                              {"payload", oursql::DataType::Varchar, 512}};
  oursql::HeapTable table(
      &pool, oursql::TableMetadata{"orphan_multi", schema, first_page});
  for (std::int64_t key = 0; key < 120; ++key) {
    auto inserted = table.InsertRow(
        {oursql::Value(key), oursql::Value(std::string(300, static_cast<char>('a' + key % 26)))});
    if (!Check(inserted.ok(), "Rows should fill multiple HeapTable pages")) return false;
  }
  auto scanned = table.Scan();
  if (!Check(scanned.ok(), "Multi-page HeapTable should scan before Destroy")) return false;
  std::unordered_set<oursql::page_id_t> table_pages;
  for (const auto &entry : scanned.value()) table_pages.insert(entry.first.page_id);
  if (!Check(table_pages.size() > 1, "Destroy test must actually span multiple pages")) {
    return false;
  }
  const auto file_size_before = std::filesystem::file_size(temp.path);
  if (!Check(table.Destroy().ok(), "Multi-page HeapTable Destroy should succeed")) return false;

  std::unordered_set<oursql::page_id_t> reused_pages;
  for (std::size_t i = 0; i < table_pages.size(); ++i) {
    auto reused = pool.NewPage();
    if (!Check(reused.ok(), "Every destroyed table page should be reusable")) return false;
    reused_pages.insert(reused.value().PageId());
  }
  if (!Check(reused_pages == table_pages,
             "NewPage should reuse the complete destroyed data-page set")) return false;
  if (!Check(std::filesystem::file_size(temp.path) == file_size_before,
             "Reusing destroyed table pages must not grow the file")) return false;
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Multi-page Destroy test should close cleanly");
}

bool TestHeapTableDestroyMiddlePinnedIsAllOrNothing() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "Middle-pinned Destroy database should open")) {
    return false;
  }
  oursql::BufferPoolManager pool(4, &disk);
  std::vector<oursql::page_id_t> pages;
  for (int i = 0; i < 3; ++i) {
    auto created = pool.NewPageGuarded();
    if (!Check(created.ok(), "Three-page Destroy chain should allocate")) return false;
    auto page = std::move(created.value());
    pages.push_back(page.PageId());
    if (!Check(oursql::SlottedPage::Initialize(page).ok(),
               "Three-page Destroy chain page should initialize")) {
      return false;
    }
  }
  for (std::size_t i = 0; i + 1 < pages.size(); ++i) {
    auto linked = pool.FetchPageWrite(pages[i]);
    if (!Check(linked.ok(), "Three-page Destroy chain page should be writable")) return false;
    auto page = std::move(linked.value());
    if (!Check(oursql::SlottedPage::SetNextPageId(page, pages[i + 1]).ok(),
               "Three-page Destroy chain should link pages")) {
      return false;
    }
  }

  oursql::HeapTable table(
      &pool, oursql::TableMetadata{"middle_pinned",
                                   oursql::Schema{{"value", oursql::DataType::Int}}, pages[0]});
  const auto file_size_before = std::filesystem::file_size(temp.path);
  {
    auto pinned_result = pool.FetchPage(pages[1]);
    if (!Check(pinned_result.ok(), "Middle page should be pinned for regression test")) {
      return false;
    }
    auto pinned = std::move(pinned_result.value());
    const auto rejected = table.Destroy();
    if (!Check(rejected.code() == oursql::ErrorCode::InvalidArgument &&
                   rejected.message().find(u8"被 pin 的页面不能删除") != std::string::npos,
               "Destroy should reject a pinned middle page before deleting anything")) {
      return false;
    }

    for (std::size_t i = 0; i < pages.size(); ++i) {
      auto fetched = pool.FetchPage(pages[i]);
      if (!Check(fetched.ok(), "Pinned Destroy failure must keep every page present")) {
        return false;
      }
      auto page = std::move(fetched.value());
      if (!Check(oursql::SlottedPage::Validate(page).ok(),
                 "Pinned Destroy failure must keep every page valid")) {
        return false;
      }
      auto next = oursql::SlottedPage::NextPageId(page);
      const auto expected = i + 1 < pages.size() ? pages[i + 1] : oursql::INVALID_PAGE_ID;
      if (!Check(next.ok() && next.value() == expected,
                 "Pinned Destroy failure must preserve the complete page chain")) {
        return false;
      }
    }
  }

  for (std::size_t i = 0; i < pages.size(); ++i) {
    auto fetched = pool.FetchPage(pages[i]);
    if (!Check(fetched.ok(), "Released middle pin should leave every page fetchable")) {
      return false;
    }
    auto page = std::move(fetched.value());
    if (!Check(oursql::SlottedPage::Validate(page).ok(),
               "Released middle pin should leave every page valid")) {
      return false;
    }
    auto next = oursql::SlottedPage::NextPageId(page);
    const auto expected = i + 1 < pages.size() ? pages[i + 1] : oursql::INVALID_PAGE_ID;
    if (!Check(next.ok() && next.value() == expected,
               "Released middle pin should preserve the complete page chain")) {
      return false;
    }
  }
  if (!Check(table.Destroy().ok(), "Destroy should succeed after releasing the middle pin")) {
    return false;
  }

  std::unordered_set<oursql::page_id_t> reused;
  for (std::size_t i = 0; i < pages.size(); ++i) {
    auto created = pool.NewPage();
    if (!Check(created.ok(), "Destroyed pages should all return to the Free List")) return false;
    reused.insert(created.value().PageId());
  }
  if (!Check(reused.size() == pages.size() &&
                 std::all_of(pages.begin(), pages.end(), [&](oursql::page_id_t page_id) {
                   return reused.count(page_id) != 0;
                 }),
             "Destroyed three-page chain should be completely reusable")) {
    return false;
  }
  if (!Check(std::filesystem::file_size(temp.path) == file_size_before,
             "Reusing destroyed pages must not expand the database file")) {
    return false;
  }
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Middle-pinned Destroy test should close cleanly");
}

bool TestHeapTableDestroyRejectsCycleBeforeDeleting() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "Cycle Destroy database should open")) return false;
  oursql::BufferPoolManager pool(3, &disk);
  oursql::page_id_t first = oursql::INVALID_PAGE_ID;
  oursql::page_id_t second = oursql::INVALID_PAGE_ID;
  {
    auto page_result = pool.NewPageGuarded();
    if (!page_result.ok()) return false;
    auto page = std::move(page_result.value());
    first = page.PageId();
    if (!oursql::SlottedPage::Initialize(page).ok()) return false;
  }
  {
    auto page_result = pool.NewPageGuarded();
    if (!page_result.ok()) return false;
    auto page = std::move(page_result.value());
    second = page.PageId();
    if (!oursql::SlottedPage::Initialize(page, first).ok()) return false;
  }
  {
    auto page_result = pool.FetchPageWrite(first);
    if (!page_result.ok()) return false;
    auto page = std::move(page_result.value());
    if (!oursql::SlottedPage::SetNextPageId(page, second).ok()) return false;
  }
  oursql::HeapTable table(
      &pool, oursql::TableMetadata{"cyclic", oursql::Schema{{"value", oursql::DataType::Int}},
                                   first});
  const auto rejected = table.Destroy();
  if (!Check(rejected.code() == oursql::ErrorCode::InvalidArgument,
             "Destroy should reject a cyclic data-page chain")) return false;
  {
    auto first_page = pool.FetchPage(first);
    auto second_page = pool.FetchPage(second);
    if (!Check(first_page.ok() && second_page.ok(),
               "Cycle validation failure must occur before deleting any page")) return false;
  }
  {
    auto page_result = pool.FetchPageWrite(second);
    if (!page_result.ok()) return false;
    auto page = std::move(page_result.value());
    if (!oursql::SlottedPage::SetNextPageId(page, oursql::INVALID_PAGE_ID).ok()) return false;
  }
  if (!Check(table.Destroy().ok(), "Destroy should succeed after repairing the cycle")) {
    return false;
  }
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Cycle Destroy test should close cleanly");
}

bool TestHeapTableReclaimsEmptyNonHeadPages() {
  TempDb temp;
  const oursql::Schema schema{{"id", oursql::DataType::Int},
                              {"payload", oursql::DataType::Varchar, 512}};
  oursql::page_id_t first_page = oursql::INVALID_PAGE_ID;

  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "Empty-page reclaim database should open")) {
      return false;
    }
    oursql::BufferPoolManager pool(4, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok(), "Empty-page reclaim Catalog should open")) return false;
    if (!Check(catalog.CreateTable(oursql::TableMetadata{
                       "reclaim_rows", schema, oursql::INVALID_PAGE_ID})
                   .ok(),
               "Empty-page reclaim table should be created")) {
      return false;
    }
    auto metadata = catalog.GetTableMetadata("reclaim_rows");
    if (!Check(metadata.ok(), "Empty-page reclaim metadata should be readable")) return false;
    first_page = metadata.value()->first_data_page_id;
    oursql::HeapTable table(&pool, *metadata.value());

    for (std::int64_t i = 0; i < 120; ++i) {
      const auto row = oursql::Row{
          oursql::Value(i), oursql::Value(std::string(300, static_cast<char>('a' + i % 26)))};
      auto rid = table.InsertRow(row);
      if (!rid.ok()) {
        std::cerr << "Empty-page reclaim insert " << i << ": "
                  << rid.status().ToString() << '\n';
        return Check(false, "Empty-page reclaim rows should insert across pages");
      }
    }
    auto scanned = table.Scan();
    if (!Check(scanned.ok(), "Empty-page reclaim table should scan before deletion")) return false;

    std::vector<oursql::page_id_t> page_order;
    std::unordered_set<oursql::page_id_t> seen_pages;
    std::map<oursql::page_id_t, std::vector<oursql::RID>> rids_by_page;
    for (const auto &entry : scanned.value()) {
      if (seen_pages.insert(entry.first.page_id).second) page_order.push_back(entry.first.page_id);
      rids_by_page[entry.first.page_id].push_back(entry.first);
    }
    if (!Check(page_order.size() >= 3, "Empty-page reclaim test should create at least three pages")) {
      return false;
    }

    const auto middle_page = page_order[1];
    for (const auto &rid : rids_by_page[middle_page]) {
      if (!Check(table.DeleteRow(rid).ok(), "Deleting all middle-page rows should succeed")) {
        return false;
      }
    }
    scanned = table.Scan();
    if (!Check(scanned.ok(), "Scan should succeed after middle-page reclaim")) return false;
    for (const auto &entry : scanned.value()) {
      if (!Check(entry.first.page_id != middle_page,
                 "Reclaimed middle page must not remain in the table chain")) {
        return false;
      }
    }

    {
      auto reused = pool.NewPage();
      if (!Check(reused.ok() && reused.value().PageId() == middle_page,
                 "Reclaimed middle page should be reused first")) {
        return false;
      }
    }
    if (!Check(pool.DeletePage(middle_page).ok(),
               "Temporary reuse of middle page should be releasable")) {
      return false;
    }

    const auto tail_page = page_order.back();
    for (const auto &rid : rids_by_page[tail_page]) {
      if (!Check(table.DeleteRow(rid).ok(), "Deleting all tail-page rows should succeed")) {
        return false;
      }
    }
    scanned = table.Scan();
    if (!Check(scanned.ok(), "Scan should succeed after tail-page reclaim")) return false;
    for (const auto &entry : scanned.value()) {
      if (!Check(entry.first.page_id != tail_page,
                 "Reclaimed tail page must not remain in the table chain")) {
        return false;
      }
    }

    for (const auto page_id : page_order) {
      if (page_id == first_page || page_id == middle_page || page_id == tail_page) continue;
      for (const auto &rid : rids_by_page[page_id]) {
        if (!Check(table.DeleteRow(rid).ok(),
                   "Deleting all remaining non-head page rows should succeed")) {
          return false;
        }
      }
    }

    for (const auto &rid : rids_by_page[first_page]) {
      if (!Check(table.DeleteRow(rid).ok(), "Deleting all head-page rows should succeed")) {
        return false;
      }
    }
    auto empty_head_scan = table.Scan();
    if (!Check(empty_head_scan.ok(), "An empty but retained head page should scan")) return false;

    if (!Check(pool.Close().ok() && disk.Close().ok(),
               "Empty-page reclaim first lifetime should close cleanly")) {
      return false;
    }
  }

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "Empty-page reclaim database should reopen")) {
    return false;
  }
  oursql::BufferPoolManager reopened_pool(4, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "Empty-page reclaim Catalog should reopen")) return false;
  auto metadata = reopened_catalog.GetTableMetadata("reclaim_rows");
  if (!Check(metadata.ok() && metadata.value()->first_data_page_id == first_page,
             "Reopen should preserve the first data-page entry")) {
    return false;
  }
  oursql::HeapTable reopened_table(&reopened_pool, *metadata.value());
  auto scanned = reopened_table.Scan();
  const bool ok = Check(scanned.ok() && scanned.value().empty(),
                        "Reopen should scan the table after all rows are deleted");
  return reopened_pool.Close().ok() && reopened_disk.Close().ok() && ok;
}

bool TestHeapTableLargePersistenceAndReclaim() {
  TempDb temp;
  const oursql::Schema schema{{"id", oursql::DataType::Int},
                              {"name", oursql::DataType::Varchar, 64}};
  oursql::page_id_t first_page = oursql::INVALID_PAGE_ID;
  oursql::page_id_t reclaimed_page = oursql::INVALID_PAGE_ID;
  std::unordered_set<std::int64_t> removed_ids;
  constexpr std::int64_t kRowCount = 5000;

  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "Large persistence database should open")) return false;
    oursql::BufferPoolManager pool(8, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok(), "Large persistence Catalog should open")) return false;
    if (!Check(catalog.CreateTable(oursql::TableMetadata{
                       "large_rows", schema, oursql::INVALID_PAGE_ID})
                   .ok(),
               "Large persistence table should be created")) {
      return false;
    }
    auto metadata = catalog.GetTableMetadata("large_rows");
    if (!Check(metadata.ok(), "Large persistence metadata should be readable")) return false;
    first_page = metadata.value()->first_data_page_id;
    oursql::HeapTable table(&pool, *metadata.value());

    for (std::int64_t id = 0; id < kRowCount; ++id) {
      auto inserted = table.InsertRow({oursql::Value(id), oursql::Value("row_" + std::to_string(id))});
      if (!Check(inserted.ok(), "5000 rows should insert across multiple pages")) return false;
    }
    auto before = table.Scan();
    if (!Check(before.ok() && before.value().size() == kRowCount,
               "5000 rows should scan completely before reclaim")) {
      return false;
    }

    std::vector<oursql::page_id_t> page_order;
    std::unordered_set<oursql::page_id_t> seen_pages;
    std::map<oursql::page_id_t, std::vector<oursql::RID>> rids_by_page;
    for (const auto &entry : before.value()) {
      if (seen_pages.insert(entry.first.page_id).second) page_order.push_back(entry.first.page_id);
      rids_by_page[entry.first.page_id].push_back(entry.first);
    }
    if (!Check(page_order.size() >= 3, "5000-row test should create multiple data pages")) return false;
    reclaimed_page = page_order[page_order.size() / 2];
    for (const auto &rid : rids_by_page[reclaimed_page]) {
      auto row = table.GetRow(rid);
      if (!Check(row.ok(), "Rows selected for full-page reclaim should be readable")) return false;
      removed_ids.insert(row.value()[0].AsInt());
      if (!Check(table.DeleteRow(rid).ok(), "Deleting a complete large-table page should succeed")) {
        return false;
      }
    }
    auto after_delete = table.Scan();
    if (!Check(after_delete.ok() && after_delete.value().size() == kRowCount - removed_ids.size(),
               "Large-table scan should skip reclaimed page rows")) {
      return false;
    }
    if (!Check(pool.Close().ok() && disk.Close().ok(),
               "Large persistence first lifetime should close cleanly")) {
      return false;
    }
  }

  oursql::DiskManager reopened_disk;
  if (!Check(reopened_disk.Open(temp.path).ok(), "Large persistence database should reopen")) return false;
  oursql::BufferPoolManager reopened_pool(8, &reopened_disk);
  oursql::Catalog reopened_catalog(&reopened_pool, &reopened_disk);
  if (!Check(reopened_catalog.Open().ok(), "Large persistence Catalog should reopen")) return false;
  auto metadata = reopened_catalog.GetTableMetadata("large_rows");
  if (!Check(metadata.ok() && metadata.value()->first_data_page_id == first_page,
             "Large persistence should preserve the first data page")) {
    return false;
  }
  oursql::HeapTable reopened_table(&reopened_pool, *metadata.value());
  auto scanned = reopened_table.Scan();
  if (!Check(scanned.ok() && scanned.value().size() == kRowCount - removed_ids.size(),
             "Reopened large table should retain every non-deleted row")) {
    return false;
  }
  std::unordered_set<std::int64_t> observed_ids;
  for (const auto &entry : scanned.value()) {
    const auto id = entry.second[0].AsInt();
    if (!Check(removed_ids.count(id) == 0 && observed_ids.insert(id).second,
               "Reopened large scan should have no deleted or duplicate ids")) {
      return false;
    }
    if (!Check(entry.second[1].AsVarchar() == "row_" + std::to_string(id),
               "Reopened large scan should preserve row contents")) {
      return false;
    }
  }
  {
    auto reused = reopened_pool.NewPage();
    if (!Check(reused.ok() && reused.value().PageId() == reclaimed_page,
               "Reclaimed large-table page should be reusable after restart")) {
      return false;
    }
  }
  if (!Check(reopened_pool.DeletePage(reclaimed_page).ok(),
             "Temporary large-table page reuse should be releasable")) {
    return false;
  }
  return reopened_pool.Close().ok() && reopened_disk.Close().ok();
}

bool TestDropIndexReusesAllPagesAcrossRestart() {
  TempDb temp;
  std::uintmax_t page_count_before = 0;
  std::uintmax_t page_count_after = 0;
  {
    oursql::DiskManager disk;
    if (!Check(disk.Open(temp.path).ok(), "Drop-index reclaim database should open")) return false;
    oursql::BufferPoolManager pool(6, &disk);
    oursql::Catalog catalog(&pool, &disk);
    if (!Check(catalog.Open().ok(), "Drop-index reclaim Catalog should open")) return false;
    if (!Check(catalog.CreateTable(oursql::TableMetadata{
                   "student", UserSchema(), oursql::INVALID_PAGE_ID}).ok(),
               "Drop-index reclaim table should be created")) return false;
    auto metadata = catalog.GetTableMetadata("student");
    if (!metadata.ok()) return false;
    oursql::HeapTable table(&pool, *metadata.value());
    for (std::int64_t key = 0; key < 1000; ++key) {
      auto inserted = table.InsertRow(
          {oursql::Value(key), oursql::Value("student_" + std::to_string(key))});
      if (!Check(inserted.ok(), "Drop-index reclaim rows should insert")) return false;
    }
    if (!Check(pool.FlushAllPages().ok(), "Table pages should flush before index measurement")) {
      return false;
    }
    page_count_before = std::filesystem::file_size(temp.path) / oursql::Page::kSize;

    oursql::IndexManager indexes(&catalog, &pool);
    if (!Check(indexes.CreateIndex("idx_student_id", "student", "id", true).ok(),
               "Drop-index reclaim index should be created")) return false;
    if (!Check(pool.FlushAllPages().ok(), "Index pages should flush before measurement")) {
      return false;
    }
    page_count_after = std::filesystem::file_size(temp.path) / oursql::Page::kSize;
    if (!Check(page_count_after > page_count_before,
               "Creating the index should allocate additional pages")) return false;
    if (!Check(indexes.DropIndex("idx_student_id").ok() &&
                   !catalog.HasIndex("idx_student_id"),
               "DropIndex should remove metadata and destroy the tree")) return false;
    if (!Check(pool.Close().ok() && disk.Close().ok(),
               "Drop-index reclaim first lifetime should close cleanly")) return false;
  }

  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "Drop-index reclaim database should reopen")) return false;
  oursql::BufferPoolManager pool(6, &disk);
  oursql::Catalog catalog(&pool, &disk);
  if (!Check(catalog.Open().ok() && !catalog.HasIndex("idx_student_id"),
             "Dropped index metadata must stay absent after restart")) return false;

  std::unordered_set<oursql::page_id_t> expected;
  for (std::uintmax_t page = page_count_before; page < page_count_after; ++page) {
    expected.insert(static_cast<oursql::page_id_t>(page));
  }
  std::unordered_set<oursql::page_id_t> reused;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    auto page = pool.NewPage();
    if (!Check(page.ok(), "Every dropped index page should be reallocated")) return false;
    reused.insert(page.value().PageId());
  }
  if (!Check(reused == expected,
             "Reallocated page ids should equal the complete dropped-index range")) return false;
  if (!Check(std::filesystem::file_size(temp.path) == page_count_after * oursql::Page::kSize,
             "Reallocating dropped-index pages must not grow the file")) return false;
  return Check(pool.Close().ok() && disk.Close().ok(),
               "Drop-index reclaim second lifetime should close cleanly");
}

bool TestConcurrentHeapScansUseCoupledPageHandoff() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "并发 HeapTable 测试数据库打开应成功")) return false;
  oursql::BufferPoolManager pool(3, &disk);
  oursql::Catalog catalog(&pool, &disk);
  if (!Check(catalog.Open().ok(), "并发 HeapTable Catalog 打开应成功")) return false;
  if (!Check(catalog.CreateTable(oursql::TableMetadata{"concurrent", UserSchema(),
                                                        oursql::INVALID_PAGE_ID})
                 .ok(),
             "并发 HeapTable 建表应成功")) return false;
  auto metadata = catalog.GetTableMetadata("concurrent");
  if (!Check(metadata.ok(), "并发 HeapTable 元数据应存在")) return false;
  oursql::HeapTable table(&pool, *metadata.value());
  for (int i = 0; i < 180; ++i) {
    auto inserted = table.InsertRow(
        {oursql::Value(static_cast<std::int64_t>(i)), oursql::Value(std::string(60, 'x'))});
    if (!Check(inserted.ok(), "并发 HeapTable 测试行插入应成功")) return false;
  }

  std::size_t first_count = 0;
  std::size_t second_count = 0;
  bool first_ok = true;
  bool second_ok = true;
  auto scan_worker = [&](std::size_t *count, bool *ok) {
    auto cursor = table.BeginScan();
    if (!cursor.ok()) {
      *ok = false;
      return;
    }
    while (true) {
      auto row = cursor.value().Next();
      if (!row.ok()) {
        *ok = false;
        return;
      }
      if (!row.value().has_value()) break;
      ++*count;
    }
  };
  std::thread first(scan_worker, &first_count, &first_ok);
  std::thread second(scan_worker, &second_count, &second_ok);
  first.join();
  second.join();
  const bool counts_ok = Check(first_ok && second_ok && first_count == 180 && second_count == 180,
                               "并发扫描应完整返回每一页的有效行");
  bool pins_zero = true;
  for (const auto &snapshot : pool.GetFrameSnapshots()) {
    if (snapshot.pin_count != 0) pins_zero = false;
  }
  const bool close_ok = Check(pins_zero, "并发扫描结束后所有 frame pin_count 应归零") &&
                        Check(pool.Close().ok() && disk.Close().ok(),
                              "并发 HeapTable 测试应正常关闭");
  return counts_ok && close_ok;
}

bool TestHeapTableSingleFrameHandoffFailsClearly() {
  TempDb temp;
  oursql::DiskManager disk;
  if (!Check(disk.Open(temp.path).ok(), "单 frame 页链测试数据库打开应成功")) return false;
  oursql::BufferPoolManager pool(1, &disk);
  oursql::page_id_t first = oursql::INVALID_PAGE_ID;
  oursql::page_id_t second = oursql::INVALID_PAGE_ID;
  {
    auto page = pool.NewPage();
    if (!Check(page.ok(), "单 frame 测试首数据页创建应成功")) return false;
    first = page.value().PageId();
    if (!Check(oursql::SlottedPage::Initialize(page.value()).ok(), "首数据页初始化应成功")) {
      return false;
    }
  }
  {
    auto page = pool.NewPage();
    if (!Check(page.ok(), "单 frame 测试后继数据页创建应成功")) return false;
    second = page.value().PageId();
    if (!Check(oursql::SlottedPage::Initialize(page.value()).ok(), "后继数据页初始化应成功")) {
      return false;
    }
  }
  {
    auto page = pool.FetchPageWrite(first);
    if (!Check(page.ok(), "单 frame 测试首数据页读取应成功")) return false;
    if (!Check(oursql::SlottedPage::SetNextPageId(page.value(), second).ok(),
               "单 frame 测试链表连接应成功")) return false;
  }

  bool got_clear_failure = false;
  {
    oursql::HeapTable table(&pool, oursql::TableMetadata{"single_frame", UserSchema(), first});
    auto cursor = table.BeginScan();
    if (!Check(cursor.ok(), "单 frame 测试扫描游标创建应成功")) return false;
    auto next = cursor.value().Next();
    got_clear_failure = !next.ok() && next.status().code() == oursql::ErrorCode::NotFound;
  }
  const bool close_ok = Check(got_clear_failure,
                              "单 frame 跨页交接应明确报告无法同时 pin 两页") &&
                        Check(pool.Close().ok() && disk.Close().ok(),
                              "单 frame 页链测试应正常关闭");
  return close_ok;
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) std::cout << "[PASS] " << name << '\n';
    else ++failures;
  };
  run("Row codec", &TestRowCodec);
  run("Catalog head is durable immediately", &TestCatalogHeadIsDurableImmediately);
  run("Catalog persistence and chain", &TestCatalogPersistenceAndChain);
  run("HeapTable pages delete and restart", &TestHeapTableAcrossPagesDeleteAndRestart);
  run("RID ownership", &TestRidOwnership);
  run("Oversized records do not allocate pages", &TestOversizedRecordsDoNotAllocatePages);
  run("Index metadata persistence", &TestIndexMetadataPersistence);
  run("IndexManager build maintain and drop", &TestIndexManagerBuildMaintainAndDrop);
  run("HeapTable Destroy single page and pinned retry",
      &TestHeapTableDestroySinglePageAndPinnedRetry);
  run("HeapTable Destroy multiple pages", &TestHeapTableDestroyMultiplePages);
  run("HeapTable Destroy middle pinned is all-or-nothing",
      &TestHeapTableDestroyMiddlePinnedIsAllOrNothing);
  run("HeapTable Destroy rejects cycle before deleting",
      &TestHeapTableDestroyRejectsCycleBeforeDeleting);
  run("HeapTable reclaims empty non-head pages", &TestHeapTableReclaimsEmptyNonHeadPages);
  run("HeapTable large persistence and reclaim", &TestHeapTableLargePersistenceAndReclaim);
  run("DropIndex reuses all pages across restart",
      &TestDropIndexReusesAllPagesAcrossRestart);
  run("Concurrent HeapTable scans use coupled page handoff",
      &TestConcurrentHeapScansUseCoupledPageHandoff);
  run("HeapTable single-frame handoff fails clearly",
      &TestHeapTableSingleFrameHandoffFailsClearly);
  if (failures == 0) {
    std::cout << "All catalog and heap tests passed\n";
    return 0;
  }
  std::cerr << failures << " catalog/heap test(s) failed\n";
  return 1;
}
