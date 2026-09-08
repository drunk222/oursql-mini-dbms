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
  return engine.Close().ok();
}

}  // namespace

int main() {
  if (!TestDatabaseBoundaries()) return 1;
  std::cout << "[PASS] Database boundary cases\n";
  return 0;
}
