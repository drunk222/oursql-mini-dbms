#include "oursql/execution/database_engine.h"

#include <chrono>
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
           ("oursql_execution_" + std::to_string(stamp) + ".oursql");
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

bool TestMinimumChainAndRestart() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 3);
    if (!Check(engine.GetInitStatus().ok(), "DatabaseEngine 初次打开应成功")) return false;
    auto result = engine.ExecuteSql(
        "CREATE TABLE student(id INT, name VARCHAR);"
        "INSERT INTO student VALUES(1, 'Alice');"
        "SELECT * FROM student;");
    if (!Check(result.ok(), "CREATE/INSERT/SELECT * 最小链路应成功")) {
      std::cerr << result.status().ToString() << '\n';
      return false;
    }
    if (!Check(result.value().column_names == std::vector<std::string>{"id", "name"},
               "SELECT * 列名应按 Schema 顺序返回")) {
      return false;
    }
    if (!Check(result.value().rows.size() == 1 && result.value().rows[0].size() == 2,
               "SELECT * 应返回一行两列")) {
      return false;
    }
    if (!Check(result.value().rows[0][0].AsInt() == 1 &&
                   result.value().rows[0][1].AsVarchar() == "Alice",
               "SELECT * 返回值应为 1 和 Alice")) {
      return false;
    }
    if (!Check(result.value().rids.size() == 1 && result.value().rids[0].page_id != 0,
               "SELECT * 应保留有效 RID")) {
      return false;
    }
    auto batch = engine.ExecuteSqlBatch("INSERT INTO student VALUES(2, 'Bob');SELECT * FROM student;");
    if (!Check(batch.ok() && batch.value().size() == 2 && batch.value()[0].affected_rows == 1 &&
                   batch.value()[1].rows.size() == 2,
               "批量接口应保留每条 SQL 的结果")) {
      return false;
    }
    if (!Check(engine.Close().ok(), "初次关闭应成功")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(), "重启后 DatabaseEngine 打开应成功")) return false;
  auto result = reopened.ExecuteSql("SELECT * FROM student;");
  const bool ok = Check(result.ok(), "重启后 SELECT * 应成功") &&
                  Check(result.value().rows.size() == 2, "重启后应保留两条已提交数据") &&
                  Check(result.value().rows[0][0].AsInt() == 1 &&
                            result.value().rows[0][1].AsVarchar() == "Alice" &&
                            result.value().rows[1][0].AsInt() == 2 &&
                            result.value().rows[1][1].AsVarchar() == "Bob",
                        "重启后应保留 Alice 和 Bob 数据");
  return reopened.Close().ok() && ok;
}

bool TestFilterProjectionDeleteAndErrors() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "算子测试数据库打开应成功")) return false;
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');"
      "INSERT INTO student VALUES(2, 'Bob');"
      "INSERT INTO student VALUES(3, 'Cara');");
  if (!Check(setup.ok() && setup.value().affected_rows == 1, "多语句建表和插入应成功")) return false;

  auto projected = engine.ExecuteSql("SELECT name FROM student WHERE id = 2;");
  if (!Check(projected.ok(), "指定列和 WHERE 查询应成功")) return false;
  if (!Check(projected.value().column_names == std::vector<std::string>{"name"} &&
                 projected.value().rows.size() == 1 &&
                 projected.value().rows[0][0].AsVarchar() == "Bob",
             "WHERE 查询应只返回 Bob 的 name")) {
    return false;
  }

  auto deleted = engine.ExecuteSql("DELETE FROM student WHERE id = 2;");
  if (!Check(deleted.ok() && deleted.value().affected_rows == 1, "DELETE 应返回一行影响数")) return false;
  auto missing = engine.ExecuteSql("SELECT * FROM student WHERE id = 2;");
  if (!Check(missing.ok() && missing.value().rows.empty(), "删除后 WHERE 查询不应返回 Bob")) return false;

  auto duplicate = engine.ExecuteSql("CREATE TABLE student(id INT, name VARCHAR);");
  if (!Check(!duplicate.ok() && duplicate.status().code() == oursql::ErrorCode::AlreadyExists &&
                 duplicate.status().message().find("Planner") != std::string::npos,
             "重复建表应返回带 Planner 上下文的错误")) {
    return false;
  }
  auto unknown_table = engine.ExecuteSql("SELECT * FROM missing;");
  if (!Check(!unknown_table.ok() && unknown_table.status().code() == oursql::ErrorCode::NotFound &&
                 unknown_table.status().message().find("Planner") != std::string::npos,
             "未知表应返回带 Planner 上下文的错误")) {
    return false;
  }
  auto unknown_column = engine.ExecuteSql("SELECT missing FROM student;");
  if (!Check(!unknown_column.ok() && unknown_column.status().code() == oursql::ErrorCode::NotFound,
             "未知列应返回明确错误")) {
    return false;
  }
  auto wrong_type = engine.ExecuteSql("INSERT INTO student VALUES('one', 'One');");
  return Check(!wrong_type.ok() && wrong_type.status().code() == oursql::ErrorCode::TypeMismatch,
               "VALUES 类型错误应返回明确错误");
}

bool TestLargeTableAndRestart() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 3);
    if (!Check(engine.GetInitStatus().ok(), "跨页测试数据库打开应成功")) return false;
    if (!Check(engine.ExecuteSql("CREATE TABLE numbers(id INT, name VARCHAR);").ok(),
               "跨页表创建应成功")) {
      return false;
    }
    for (int i = 0; i < 140; ++i) {
      const auto sql = "INSERT INTO numbers VALUES(" + std::to_string(i) + ", 'number_" +
                       std::to_string(i) + "');";
      if (!Check(engine.ExecuteSql(sql).ok(), "跨页插入应成功")) return false;
    }
    auto result = engine.ExecuteSql("SELECT id, name FROM numbers;");
    if (!Check(result.ok() && result.value().rows.size() == 140, "跨页 SELECT 数量应正确")) return false;
    for (int i = 0; i < 140; ++i) {
      if (!Check(result.value().rows[static_cast<std::size_t>(i)][0].AsInt() == i &&
                     result.value().rows[static_cast<std::size_t>(i)][1].AsVarchar() ==
                         "number_" + std::to_string(i),
                 "跨页 SELECT 内容应保持顺序")) {
        return false;
      }
    }
    if (!Check(engine.Close().ok(), "跨页数据库关闭应成功")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(), "跨页数据库重启应成功")) return false;
  auto result = reopened.ExecuteSql("SELECT * FROM numbers WHERE id = 139;");
  const bool ok = Check(result.ok() && result.value().rows.size() == 1,
                        "重启后跨页 WHERE 应找到一行") &&
                  Check(result.value().rows[0][1].AsVarchar() == "number_139",
                        "重启后跨页内容应正确");
  return reopened.Close().ok() && ok;
}

}  // namespace

int main() {
  if (TestMinimumChainAndRestart()) {
    std::cout << "[PASS] Minimum execution chain and restart\n";
  } else {
    return 1;
  }
  if (TestFilterProjectionDeleteAndErrors()) {
    std::cout << "[PASS] Filter projection delete and errors\n";
  } else {
    return 1;
  }
  if (TestLargeTableAndRestart()) {
    std::cout << "[PASS] Large table and restart\n";
    return 0;
  }
  return 1;
}
