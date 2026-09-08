#include "oursql/execution/database_engine.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace {

struct TempDb {
  std::filesystem::path path;

  TempDb() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() /
           ("oursql_edge_" + std::to_string(stamp) + ".oursql");
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

bool TestDatabaseBoundaries() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "边界测试数据库打开应成功")) return false;

  auto empty = engine.ExecuteSql("");
  if (!Check(!empty.ok() && empty.status().code() == oursql::ErrorCode::InvalidArgument,
             "空 SQL 应返回错误而不是崩溃")) return false;
  if (!Check(engine.ExecuteSql("CREATE TABLE edge(id INT, name VARCHAR(4));").ok(),
             "边界表创建应成功")) {
    return false;
  }
  auto negative = engine.ExecuteSql("INSERT INTO edge VALUES(-7, 'ok');");
  if (!Check(negative.ok() && negative.value().affected_rows == 1, "负整数应可插入")) return false;
  auto too_long = engine.ExecuteSql("INSERT INTO edge VALUES(1, 'longer');");
  if (!Check(!too_long.ok() && too_long.status().code() == oursql::ErrorCode::InvalidArgument,
             "超长 VARCHAR 应被拒绝")) {
    return false;
  }
  if (!Check(engine.ExecuteSql("CREATE TABLE empty_table(id INT);").ok(), "空表创建应成功")) return false;
  auto empty_result = engine.ExecuteSql("SELECT * FROM empty_table;");
  if (!Check(empty_result.ok() && empty_result.value().rows.empty(), "空表 SELECT 应返回空结果")) return false;

  auto deleted = engine.ExecuteSql("DELETE FROM edge WHERE id = -7;");
  if (!Check(deleted.ok() && deleted.value().affected_rows == 1, "负数 WHERE 删除应成功")) return false;
  auto repeated = engine.ExecuteSql("DELETE FROM edge WHERE id = -7;");
  if (!Check(repeated.ok() && repeated.value().affected_rows == 0, "重复删除应安全返回零影响行")) return false;
  const std::string nihao{static_cast<char>(0xE4), static_cast<char>(0xBD), static_cast<char>(0xA0),
                          static_cast<char>(0xE5), static_cast<char>(0xA5), static_cast<char>(0xBD)};
  if (!Check(nihao.size() == 6, "UTF-8 测试字符串应按 6 个字节构造")) return false;
  if (!Check(engine.ExecuteSql("CREATE TABLE utf8_ok(name VARCHAR(6));").ok(),
             "VARCHAR(6) 表创建应成功")) return false;
  auto utf8_insert = engine.ExecuteSql("INSERT INTO utf8_ok VALUES('" + nihao + "');");
  if (!Check(utf8_insert.ok(), "6 字节 UTF-8 字符串应被 VARCHAR(6) 接受")) return false;
  if (!Check(engine.ExecuteSql("CREATE TABLE utf8_short(name VARCHAR(5));").ok(),
             "VARCHAR(5) 表创建应成功")) return false;
  auto utf8_rejected = engine.ExecuteSql("INSERT INTO utf8_short VALUES('" + nihao + "');");
  if (!Check(!utf8_rejected.ok() && utf8_rejected.status().code() == oursql::ErrorCode::InvalidArgument,
             "6 字节 UTF-8 字符串应被 VARCHAR(5) 拒绝")) return false;
  if (!Check(engine.Close().ok(), "边界测试数据库关闭应成功")) return false;

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(), "UTF-8 测试数据库重启应成功")) return false;
  auto utf8_result = reopened.ExecuteSql("SELECT * FROM utf8_ok;");
  const bool ok = Check(utf8_result.ok() && utf8_result.value().rows.size() == 1,
                        "重启后 UTF-8 查询应返回一行") &&
                  Check(utf8_result.value().rows[0][0].AsVarchar() == nihao,
                        "重启后 UTF-8 字节内容应完全一致");
  return reopened.Close().ok() && ok;
}

}  // namespace

int main() {
  if (!TestDatabaseBoundaries()) return 1;
  std::cout << "[PASS] Database boundary cases\n";
  return 0;
}
