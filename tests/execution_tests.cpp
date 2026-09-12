#include "oursql/execution/database_engine.h"

#include <algorithm>
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

bool TestOrderGroupAndUpdate() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "扩展执行测试数据库打开应成功")) return false;
  auto setup = engine.ExecuteSql(
      "CREATE TABLE scores(id INT, team VARCHAR(8), score INT);"
      "INSERT INTO scores VALUES(1, 'red', 10);"
      "INSERT INTO scores VALUES(2, 'blue', 30);"
      "INSERT INTO scores VALUES(3, 'red', 30);"
      "INSERT INTO scores VALUES(4, 'blue', 20);");
  if (!Check(setup.ok(), "ORDER/GROUP/UPDATE 测试数据应创建成功")) return false;

  auto ordered = engine.ExecuteSql(
      "SELECT id FROM scores ORDER BY score DESC, id ASC;");
  if (!Check(ordered.ok() && ordered.value().rows.size() == 4,
             "多键 ORDER BY 应执行成功")) return false;
  if (!Check(ordered.value().rows[0][0].AsInt() == 2 &&
                 ordered.value().rows[1][0].AsInt() == 3 &&
                 ordered.value().rows[2][0].AsInt() == 4 &&
                 ordered.value().rows[3][0].AsInt() == 1,
             "ORDER BY 应支持投影外排序列和 ASC/DESC")) return false;

  auto grouped = engine.ExecuteSql(
      "SELECT team FROM scores GROUP BY team ORDER BY team DESC;");
  if (!Check(grouped.ok() && grouped.value().rows.size() == 2 &&
                 grouped.value().rows[0][0].AsVarchar() == "red" &&
                 grouped.value().rows[1][0].AsVarchar() == "blue",
             "GROUP BY 应按分组键去重并可继续排序")) return false;

  auto updated = engine.ExecuteSql(
      "UPDATE scores SET score = score + 5, team = 'green' WHERE id = 1;");
  if (!Check(updated.ok() && updated.value().affected_rows == 1 &&
                 updated.value().rids.size() == 1,
             "带 WHERE 和表达式的 UPDATE 应更新一行")) return false;
  auto selected = engine.ExecuteSql("SELECT team, score FROM scores WHERE id = 1;");
  if (!Check(selected.ok() && selected.value().rows.size() == 1 &&
                 selected.value().rows[0][0].AsVarchar() == "green" &&
                 selected.value().rows[0][1].AsInt() == 15,
             "UPDATE 多列赋值应基于原行求值并持久化")) return false;

  auto update_all = engine.ExecuteSql("UPDATE scores SET score = score * 2;");
  if (!Check(update_all.ok() && update_all.value().affected_rows == 4,
             "无 WHERE UPDATE 应更新整表")) return false;
  auto divide_zero = engine.ExecuteSql("UPDATE scores SET score = score / 0 WHERE id = 2;");
  if (!Check(!divide_zero.ok() &&
                 divide_zero.status().code() == oursql::ErrorCode::InvalidArgument,
             "UPDATE 运行时除零应返回错误且不修改行")) return false;
  auto unchanged = engine.ExecuteSql("SELECT score FROM scores WHERE id = 2;");
  return Check(unchanged.ok() && unchanged.value().rows.size() == 1 &&
                   unchanged.value().rows[0][0].AsInt() == 60,
               "UPDATE 求值失败时目标行应保持不变");
}

bool TestExtendedInsertCompileOnly() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "扩展 INSERT 测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');");
  if (!Check(setup.ok(), "原有单行全列 INSERT 应继续可执行")) return false;

  auto specified = engine.ExecuteSql(
      "INSERT INTO student (name, id) VALUES('Bob', 2);");
  if (!Check(!specified.ok() &&
                 specified.status().code() == oursql::ErrorCode::NotImplemented &&
                 specified.status().message().find("Execution") !=
                     std::string::npos,
             "指定列 INSERT 应完成编译后明确拒绝执行")) {
    return false;
  }

  auto multiple = engine.ExecuteSql(
      "INSERT INTO student VALUES(2, 'Bob'), (3, 'Cara');");
  if (!Check(!multiple.ok() &&
                 multiple.status().code() == oursql::ErrorCode::NotImplemented &&
                 multiple.status().message().find("Execution") !=
                     std::string::npos,
             "多行 INSERT 应完成编译后明确拒绝执行")) {
    return false;
  }

  auto rows = engine.ExecuteSql("SELECT * FROM student;");
  return Check(rows.ok() && rows.value().rows.size() == 1 &&
                   rows.value().rows[0][1].AsVarchar() == "Alice",
               "未实现的扩展 INSERT 不得产生部分写入");
}

bool TestDistinctAndLimitCompileOnly() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "DISTINCT/LIMIT 测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');");
  if (!Check(setup.ok(), "DISTINCT/LIMIT 测试初始化应成功")) return false;

  auto distinct = engine.ExecuteSql("SELECT DISTINCT name FROM student;");
  if (!Check(!distinct.ok() &&
                 distinct.status().code() == oursql::ErrorCode::NotImplemented &&
                 distinct.status().message().find("Execution") !=
                     std::string::npos,
             "DISTINCT 应完成编译后明确拒绝执行")) {
    return false;
  }

  auto limit = engine.ExecuteSql("SELECT * FROM student LIMIT 1;");
  return Check(!limit.ok() &&
                   limit.status().code() == oursql::ErrorCode::NotImplemented &&
                   limit.status().message().find("Execution") !=
                       std::string::npos,
               "LIMIT 应完成编译后明确拒绝执行");
}

bool TestDropTableExecutionAndAliases() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "DROP/AS 测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');");
  if (!Check(setup.ok(), "DROP/AS 测试初始化应成功")) return false;

  auto column_alias =
      engine.ExecuteSql("SELECT id AS user_id FROM student;");
  if (!Check(!column_alias.ok() &&
                 column_alias.status().code() ==
                     oursql::ErrorCode::NotImplemented &&
                 column_alias.status().message().find("Execution") !=
                     std::string::npos,
             "列别名应完成编译后明确拒绝执行")) {
    return false;
  }

  auto table_alias =
      engine.ExecuteSql("SELECT id FROM student AS s;");
  if (!Check(!table_alias.ok() &&
                 table_alias.status().code() ==
                     oursql::ErrorCode::NotImplemented,
             "表别名应完成编译后明确拒绝执行")) {
    return false;
  }

  std::string more_rows;
  for (int id = 2; id <= 300; ++id) {
    more_rows += "INSERT INTO student VALUES(" + std::to_string(id) + ",'user_" +
                 std::to_string(id) + "');";
  }
  if (!Check(engine.ExecuteSqlBatch(more_rows).ok() &&
                 engine.ExecuteSql("CREATE UNIQUE INDEX idx_student_id ON student(id);").ok() &&
                 engine.Flush().ok(),
             "DROP TABLE前应建立多页表和索引")) return false;
  const auto size_before_drop = std::filesystem::file_size(temp.path);

  auto dropped = engine.ExecuteSql("DROP TABLE student;");
  if (!Check(dropped.ok(), "DROP TABLE应删除索引、数据页和表元数据")) return false;
  const auto tables = engine.ListTables();
  bool student_exists = false;
  for (const auto &table : tables) {
    if (table.name == "student") student_exists = true;
  }
  auto rows = engine.ExecuteSql("SELECT * FROM student;");
  if (!Check(!student_exists && !rows.ok() && rows.status().code() == oursql::ErrorCode::NotFound,
             "DROP TABLE后Catalog和查询入口都应消失")) return false;

  if (!Check(engine.ExecuteSql("CREATE TABLE learner(id INT, name VARCHAR);").ok(),
             "DROP后应能创建替代表")) return false;
  std::string replacement_rows;
  for (int id = 1; id <= 300; ++id) {
    replacement_rows += "INSERT INTO learner VALUES(" + std::to_string(id) + ",'user_" +
                        std::to_string(id) + "');";
  }
  if (!Check(engine.ExecuteSqlBatch(replacement_rows).ok() &&
                 engine.ExecuteSql("CREATE UNIQUE INDEX idx_learner_id ON learner(id);").ok() &&
                 engine.Flush().ok(),
             "DROP后应复用页面建立等规模表和索引")) return false;
  if (!Check(std::filesystem::file_size(temp.path) <= size_before_drop,
             "DROP TABLE释放的数据页和索引页应由Free List复用")) return false;
  if (!Check(engine.Close().ok(), "DROP TABLE测试关闭应成功")) return false;

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(), "DROP TABLE测试重启应成功")) return false;
  auto old_table = reopened.ExecuteSql("SELECT * FROM student;");
  auto replacement = reopened.ExecuteSql("SELECT * FROM learner WHERE id = 300;");
  return Check(!old_table.ok() && replacement.ok() && replacement.value().rows.size() == 1,
               "重启后旧表和索引不应恢复，替代表索引应可查询");
}

bool TestAggregateAndHavingCompileOnly() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(),
             "聚合/HAVING 测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');"
      "INSERT INTO student VALUES(2, 'Alice');");
  if (!Check(setup.ok(), "聚合/HAVING 测试初始化应成功")) return false;

  auto aggregate = engine.ExecuteSql("SELECT COUNT(*) FROM student;");
  if (!Check(!aggregate.ok() &&
                 aggregate.status().code() ==
                     oursql::ErrorCode::NotImplemented &&
                 aggregate.status().message().find("Execution") !=
                     std::string::npos,
             "COUNT 应完成编译后明确拒绝执行")) {
    return false;
  }

  auto having = engine.ExecuteSql(
      "SELECT name, COUNT(*) FROM student "
      "GROUP BY name HAVING COUNT(*) >= 2;");
  if (!Check(!having.ok() &&
                 having.status().code() ==
                     oursql::ErrorCode::NotImplemented &&
                 having.status().message().find("Execution") !=
                     std::string::npos,
             "聚合 HAVING 应完成编译后明确拒绝执行")) {
    return false;
  }

  auto rows = engine.ExecuteSql("SELECT * FROM student;");
  return Check(rows.ok() && rows.value().rows.size() == 2,
               "未实现的聚合查询不得影响原始数据");
}

bool TestIndexMaintenanceAndExplain() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 4);
  if (!Check(engine.GetInitStatus().ok(), "索引测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR(16));"
      "INSERT INTO student VALUES(1, 'Alice');"
      "INSERT INTO student VALUES(2, 'Bob');");
  if (!Check(setup.ok(), "索引测试数据创建应成功")) return false;

  auto before = engine.ExecuteSql(
      "EXPLAIN SELECT * FROM student WHERE id = 1;");
  if (!Check(before.ok() && before.value().rows.size() == 1 &&
                 before.value().rows[0][0].AsVarchar().find("SeqScanPlan") !=
                     std::string::npos,
             "无索引 EXPLAIN 应显示 SeqScanPlan")) {
    return false;
  }

  auto created = engine.ExecuteSql(
      "CREATE UNIQUE INDEX idx_student_id ON student(id);");
  if (!Check(created.ok(), "CREATE UNIQUE INDEX 应执行成功")) return false;

  auto indexed_explain = engine.ExecuteSql(
      "EXPLAIN SELECT * FROM student WHERE id = 1;");
  if (!Check(indexed_explain.ok() &&
                 indexed_explain.value().rows[0][0].AsVarchar().find(
                     "IndexScanPlan") != std::string::npos,
             "有索引 EXPLAIN 应显示 IndexScanPlan")) {
    return false;
  }

  auto found = engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
  if (!Check(found.ok() && found.value().rows.size() == 1 &&
                 found.value().rows[0][1].AsVarchar() == "Alice",
             "IndexScan 点查应返回正确行")) {
    return false;
  }

  auto inserted = engine.ExecuteSql(
      "INSERT INTO student VALUES(3, 'Cara');");
  if (!Check(inserted.ok(), "INSERT 后索引维护应成功")) return false;
  auto inserted_lookup =
      engine.ExecuteSql("SELECT * FROM student WHERE id = 3;");
  if (!Check(inserted_lookup.ok() &&
                 inserted_lookup.value().rows.size() == 1 &&
                 inserted_lookup.value().rows[0][1].AsVarchar() == "Cara",
             "INSERT 后新键应可通过索引查询")) {
    return false;
  }

  auto updated = engine.ExecuteSql(
      "UPDATE student SET id = 4 WHERE id = 3;");
  if (!Check(updated.ok() && updated.value().affected_rows == 1,
             "UPDATE 后索引维护应成功")) {
    return false;
  }
  auto old_key = engine.ExecuteSql("SELECT * FROM student WHERE id = 3;");
  auto new_key = engine.ExecuteSql("SELECT * FROM student WHERE id = 4;");
  if (!Check(old_key.ok() && old_key.value().rows.empty() &&
                 new_key.ok() && new_key.value().rows.size() == 1,
             "UPDATE 后旧键应消失、新键应可查询")) {
    return false;
  }

  auto deleted = engine.ExecuteSql("DELETE FROM student WHERE id = 4;");
  if (!Check(deleted.ok() && deleted.value().affected_rows == 1,
             "DELETE 后索引维护应成功")) {
    return false;
  }
  auto deleted_lookup =
      engine.ExecuteSql("SELECT * FROM student WHERE id = 4;");
  if (!Check(deleted_lookup.ok() && deleted_lookup.value().rows.empty(),
             "DELETE 后索引项应被移除")) {
    return false;
  }

  auto duplicate = engine.ExecuteSql(
      "INSERT INTO student VALUES(1, 'Duplicate');");
  if (!Check(!duplicate.ok() &&
                 duplicate.status().code() ==
                     oursql::ErrorCode::AlreadyExists,
             "唯一索引重复键应拒绝 INSERT")) {
    return false;
  }
  auto after_duplicate =
      engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
  if (!Check(after_duplicate.ok() &&
                 after_duplicate.value().rows.size() == 1 &&
                 after_duplicate.value().rows[0][1].AsVarchar() == "Alice",
             "唯一索引失败后不应残留表行或索引项")) {
    return false;
  }

  auto update_conflict = engine.ExecuteSql(
      "UPDATE student SET id = 1 WHERE id = 2;");
  if (!Check(!update_conflict.ok() &&
                 update_conflict.status().code() ==
                     oursql::ErrorCode::AlreadyExists,
             "UPDATE 产生唯一键冲突时应整体失败")) {
    return false;
  }
  auto original_one =
      engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
  auto original_two =
      engine.ExecuteSql("SELECT * FROM student WHERE id = 2;");
  if (!Check(original_one.ok() && original_one.value().rows.size() == 1 &&
                 original_two.ok() && original_two.value().rows.size() == 1 &&
                 original_one.value().rows[0][1].AsVarchar() == "Alice" &&
                 original_two.value().rows[0][1].AsVarchar() == "Bob",
             "UPDATE 索引失败后应恢复旧行和旧索引")) {
    return false;
  }

  auto dropped = engine.ExecuteSql("DROP INDEX idx_student_id;");
  if (!Check(dropped.ok(), "DROP INDEX 应执行成功")) return false;
  auto after_drop = engine.ExecuteSql(
      "EXPLAIN SELECT * FROM student WHERE id = 1;");
  return Check(after_drop.ok() && after_drop.value().rows.size() == 1 &&
                   after_drop.value().rows[0][0].AsVarchar().find(
                       "SeqScanPlan") != std::string::npos,
               "DROP INDEX 后 EXPLAIN 应回退到 SeqScanPlan");
}

bool TestStatisticsResetKeepsWarmPages() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 8);
  if (!Check(engine.GetInitStatus().ok(), "Statistics test engine should open")) return false;
  auto setup = engine.ExecuteSql(
      "CREATE TABLE stats_table(id INT, name VARCHAR);"
      "INSERT INTO stats_table VALUES(1, 'warm');"
      "SELECT * FROM stats_table;");
  if (!Check(setup.ok() && setup.value().rows.size() == 1,
             "Statistics test should create and warm a table page")) return false;
  const auto before = engine.GetStatistics();
  if (!Check(before.buffer_accesses > 0 &&
                 (before.disk_reads > 0 || before.disk_writes > 0),
             "Storage counters should be nonzero before reset")) return false;

  if (!Check(engine.ResetStatistics().ok(), "Unified statistics reset should succeed")) {
    return false;
  }
  const auto reset = engine.GetStatistics();
  if (!Check(reset.buffer_accesses == 0 && reset.buffer_hits == 0 &&
                 reset.buffer_misses == 0 && reset.buffer_hit_rate == 0.0 &&
                 reset.evictions == 0 && reset.disk_reads == 0 &&
                 reset.disk_writes == 0,
             "Unified reset should clear every cumulative counter")) return false;

  auto warm_read = engine.ExecuteSql("SELECT * FROM stats_table;");
  if (!Check(warm_read.ok() && warm_read.value().rows.size() == 1,
             "The table should remain readable after resetting counters")) return false;
  const auto after = engine.GetStatistics();
  if (!Check(after.buffer_accesses > 0 && after.buffer_hits > 0 &&
                 after.buffer_misses == 0 && after.disk_reads == 0,
             "Reset should preserve warm BufferPool pages")) return false;
  if (!Check(engine.Close().ok(), "Statistics test engine should close")) return false;
  return Check(engine.ResetStatistics().code() == oursql::ErrorCode::InvalidArgument,
               "ResetStatistics should reject a closed engine");
}

std::vector<std::string> CanonicalRows(const oursql::ExecutionResult &result) {
  std::vector<std::string> rows;
  rows.reserve(result.rows.size());
  for (const auto &row : result.rows) {
    std::string encoded;
    for (const auto &value : row) {
      encoded += value.ToString();
      encoded.push_back('\x1f');
    }
    rows.push_back(std::move(encoded));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

bool TestIndexSqlRestartAndConsistency() {
  TempDb temp;
  std::vector<std::string> sequential_rows;
  {
    oursql::DatabaseEngine engine(temp.path, 8);
    if (!Check(engine.GetInitStatus().ok(), "SQL索引验收数据库应打开")) return false;
    if (!Check(engine.ExecuteSql(
                   "CREATE TABLE student(id INT, group_id INT, name VARCHAR(32));").ok(),
               "SQL索引验收建表应成功")) return false;
    for (std::int64_t first = 0; first < 600; first += 100) {
      std::string sql;
      for (std::int64_t id = first; id < first + 100; ++id) {
        sql += "INSERT INTO student VALUES(" + std::to_string(id) + "," +
               std::to_string(id % 5) + ",'student_" + std::to_string(id) + "');";
      }
      if (!Check(engine.ExecuteSqlBatch(sql).ok(), "600行测试数据批量插入应成功")) return false;
    }
    auto sequential = engine.ExecuteSql("SELECT * FROM student WHERE group_id = 3;");
    if (!Check(sequential.ok() && sequential.value().rows.size() == 120,
               "无索引SeqScan应返回完整重复Key集合")) return false;
    sequential_rows = CanonicalRows(sequential.value());
    if (!Check(engine.ExecuteSql("CREATE UNIQUE INDEX idx_student_id ON student(id);").ok() &&
                   engine.ExecuteSql("CREATE INDEX idx_student_group ON student(group_id);").ok(),
               "一张表创建唯一和非唯一两个索引应成功")) return false;
    auto indexed = engine.ExecuteSql("SELECT * FROM student WHERE group_id = 3;");
    if (!Check(indexed.ok() && CanonicalRows(indexed.value()) == sequential_rows,
               "同一查询的SeqScan与IndexScan完整结果集合应一致")) return false;
    auto duplicate_key = engine.ExecuteSql("SELECT * FROM student WHERE group_id = 3;");
    if (!Check(duplicate_key.ok() && duplicate_key.value().rows.size() == 120,
               "非唯一索引同一Key应返回全部RID")) return false;
    for (const auto key : {0, 599, 9999}) {
      auto result = engine.ExecuteSql(
          "SELECT * FROM student WHERE id = " + std::to_string(key) + ";");
      const auto expected = key == 9999 ? 0U : 1U;
      if (!Check(result.ok() && result.value().rows.size() == expected,
                 "索引应正确处理最小Key、最大Key和不存在Key")) return false;
    }
    if (!Check(engine.Close().ok(), "SQL索引首次关闭应成功")) return false;
  }

  {
    oursql::DatabaseEngine engine(temp.path, 8);
    if (!Check(engine.GetInitStatus().ok(), "SQL索引首次重启应成功")) return false;
    auto explain = engine.ExecuteSql("EXPLAIN SELECT * FROM student WHERE id = 599;");
    if (!Check(explain.ok() && explain.value().rows[0][0].AsVarchar().find("IndexScanPlan") !=
                                      std::string::npos,
               "重启后SQL仍应选择IndexScan")) return false;
    if (!Check(engine.ExecuteSql("INSERT INTO student VALUES(700,3,'restart');").ok(),
               "重启后INSERT应维护两个索引")) return false;
    if (!Check(engine.ExecuteSql("UPDATE student SET group_id = 4 WHERE id = 700;").ok(),
               "重启后UPDATE应维护两个索引")) return false;
    auto updated = engine.ExecuteSql("SELECT * FROM student WHERE id = 700;");
    if (!Check(updated.ok() && updated.value().rows.size() == 1 &&
                   updated.value().rows[0][1].AsInt() == 4,
               "UPDATE后唯一索引应指向新RID且非唯一索引Key应更新")) return false;
    if (!Check(engine.ExecuteSql("DELETE FROM student WHERE id = 700;").ok(),
               "重启后DELETE应维护两个索引")) return false;
    auto bulk_delete = engine.ExecuteSql("DELETE FROM student WHERE group_id = 1;");
    if (!bulk_delete.ok()) {
      std::cerr << "bulk delete status: " << bulk_delete.status().ToString() << '\n';
    } else if (bulk_delete.value().affected_rows != 120) {
      std::cerr << "bulk delete affected rows: " << bulk_delete.value().affected_rows << '\n';
    }
    if (!Check(bulk_delete.ok() && bulk_delete.value().affected_rows == 120,
               "大量删除应触发B+树合并并维护两个索引")) return false;
    if (!Check(engine.Close().ok(), "SQL索引第二次关闭应成功")) return false;
  }

  {
    oursql::DatabaseEngine engine(temp.path, 8);
    if (!Check(engine.GetInitStatus().ok(), "SQL索引第二次重启应成功")) return false;
    auto deleted = engine.ExecuteSql("SELECT * FROM student WHERE group_id = 1;");
    auto removed_update = engine.ExecuteSql("SELECT * FROM student WHERE id = 700;");
    if (!Check(deleted.ok() && deleted.value().rows.empty() && removed_update.ok() &&
                   removed_update.value().rows.empty(),
               "第二次重启后增删改索引状态应一致")) return false;

    if (!Check(engine.ExecuteSql("CREATE TABLE duplicate_ids(id INT);").ok() &&
                   engine.ExecuteSql("INSERT INTO duplicate_ids VALUES(1);INSERT INTO duplicate_ids VALUES(1);").ok(),
               "唯一索引失败场景数据应建立")) return false;
    auto rejected = engine.ExecuteSql(
        "CREATE UNIQUE INDEX idx_duplicate_ids ON duplicate_ids(id);");
    auto fallback = engine.ExecuteSql(
        "EXPLAIN SELECT * FROM duplicate_ids WHERE id = 1;");
    if (!Check(!rejected.ok() && rejected.status().code() == oursql::ErrorCode::AlreadyExists &&
                   fallback.ok() && fallback.value().rows[0][0].AsVarchar().find("SeqScanPlan") !=
                                        std::string::npos,
               "已有重复数据创建唯一索引应失败且不遗留元数据")) return false;

    if (!Check(engine.Flush().ok(), "DROP INDEX复用测试预刷新应成功")) return false;
    const auto size_before_drop = std::filesystem::file_size(temp.path);
    if (!Check(engine.ExecuteSql("DROP INDEX idx_student_group;").ok(),
               "SQL DROP INDEX应成功")) return false;
    if (!Check(engine.ExecuteSql("CREATE INDEX idx_student_group_rebuilt ON student(group_id);").ok() &&
                   engine.Flush().ok(),
               "DROP后重建等规模索引应成功")) return false;
    const auto size_after_rebuild = std::filesystem::file_size(temp.path);
    if (!Check(size_after_rebuild <= size_before_drop,
               "DROP INDEX释放页面应由Free List复用而不扩展数据库文件")) return false;
    if (!Check(engine.Close().ok(), "SQL索引最终关闭应成功")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 8);
  if (!Check(reopened.GetInitStatus().ok(), "重建索引后最终重启应成功")) return false;
  auto explain = reopened.ExecuteSql("EXPLAIN SELECT * FROM student WHERE group_id = 3;");
  return Check(explain.ok() && explain.value().rows[0][0].AsVarchar().find(
                                   "idx_student_group_rebuilt") != std::string::npos,
               "最终重启应恢复DROP后重建的索引元数据");
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
  } else {
    return 1;
  }
  if (TestExtendedInsertCompileOnly()) {
    std::cout << "[PASS] Extended INSERT compile-only\n";
  } else {
    return 1;
  }
  if (TestDistinctAndLimitCompileOnly()) {
    std::cout << "[PASS] DISTINCT and LIMIT compile-only\n";
  } else {
    return 1;
  }
  if (TestDropTableExecutionAndAliases()) {
    std::cout << "[PASS] DROP TABLE execution and AS compile-only\n";
  } else {
    return 1;
  }
  if (TestAggregateAndHavingCompileOnly()) {
    std::cout << "[PASS] Aggregate and HAVING compile-only\n";
  } else {
    return 1;
  }
  if (TestIndexMaintenanceAndExplain()) {
    std::cout << "[PASS] Index maintenance and EXPLAIN\n";
  } else {
    return 1;
  }
  if (TestStatisticsResetKeepsWarmPages()) {
    std::cout << "[PASS] Statistics reset keeps warm pages\n";
  } else {
    return 1;
  }
  if (TestIndexSqlRestartAndConsistency()) {
    std::cout << "[PASS] Index SQL restart and consistency\n";
  } else {
    return 1;
  }
  if (!TestOrderGroupAndUpdate()) return 1;
  std::cout << "[PASS] ORDER BY, GROUP BY and UPDATE execution\n";
  return 0;
}
