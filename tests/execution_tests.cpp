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

bool TestWebFacingDatabaseContracts() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 3);
    if (!Check(engine.GetInitStatus().ok(),
               "Web-facing contract database should open")) {
      return false;
    }
    auto results = engine.ExecuteSqlBatch(
        "CREATE TABLE zeta(id INT);"
        "CREATE TABLE student(id INT, name VARCHAR);"
        "INSERT INTO student VALUES(1, 'Alice');"
        "INSERT INTO student VALUES(2, 'Bob');"
        "SELECT * FROM student;"
        "SELECT name FROM student WHERE id = 2;"
        "DELETE FROM student WHERE id = 1;"
        "SELECT * FROM student;");
    if (!Check(results.ok() && results.value().size() == 8,
               "Web-facing CRUD batch should return one result per statement")) {
      return false;
    }
    const auto &projected = results.value()[5];
    const auto &deleted = results.value()[6];
    const auto &remaining = results.value()[7];
    if (!Check(projected.column_names == std::vector<std::string>{"name"} &&
                   projected.rows.size() == 1 &&
                   projected.rows[0][0].AsVarchar() == "Bob",
               "Web-facing projection should expose stable column names and values") ||
        !Check(deleted.affected_rows == 1,
               "Web-facing DELETE should expose the affected row count") ||
        !Check(remaining.column_names == std::vector<std::string>{"id", "name"} &&
                   remaining.rows.size() == 1 &&
                   remaining.rows[0].size() == remaining.column_names.size() &&
                   remaining.rows[0][0].AsInt() == 2 &&
                   remaining.rows[0][1].AsVarchar() == "Bob",
               "Web-facing final SELECT should contain only Bob")) {
      return false;
    }

    auto tables = engine.ListTables();
    if (!Check(tables.size() == 2 && tables[0].name == "student" &&
                   tables[1].name == "zeta",
               "ListTables should return stable name-sorted metadata copies")) {
      return false;
    }
    tables[0].name = "mutated-client-copy";
    const auto tables_again = engine.ListTables();
    if (!Check(tables_again[0].name == "student",
               "ListTables result should not expose mutable Catalog state")) {
      return false;
    }

    auto metadata = engine.GetTableMetadata("student");
    auto missing = engine.GetTableMetadata("missing");
    if (!Check(metadata.ok() && metadata.value().schema.size() == 2 &&
                   metadata.value().schema.At(0).name == "id" &&
                   metadata.value().schema.At(0).type == oursql::DataType::Int &&
                   metadata.value().schema.At(1).name == "name" &&
                   metadata.value().schema.At(1).type == oursql::DataType::Varchar,
               "GetTableMetadata should preserve column order and types") ||
        !Check(!missing.ok() &&
                   missing.status().code() == oursql::ErrorCode::NotFound,
               "GetTableMetadata should report NotFound for a missing table")) {
      return false;
    }
    metadata.value().name = "mutated-metadata-copy";
    auto metadata_again = engine.GetTableMetadata("student");
    if (!Check(metadata_again.ok() && metadata_again.value().name == "student",
               "GetTableMetadata should return an independent value copy")) {
      return false;
    }
    if (!Check(engine.Close().ok(), "Web-facing contract database should close")) {
      return false;
    }
  }

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(),
             "Web-facing contract database should reopen")) {
    return false;
  }
  auto row = reopened.ExecuteSql("SELECT * FROM student WHERE id = 2;");
  return Check(row.ok() && row.value().rows.size() == 1 &&
                   row.value().rows[0][1].AsVarchar() == "Bob",
               "Web-facing database results should persist across restart") &&
         reopened.Close().ok();
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

bool TestExtendedInsertExecution() {
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
  if (!Check(specified.ok() && specified.value().affected_rows == 1,
             "指定列 INSERT 应按 Schema 顺序写入")) {
    return false;
  }

  auto multiple = engine.ExecuteSql(
      "INSERT INTO student VALUES(2, 'Bob'), (3, 'Cara');");
  if (!Check(multiple.ok() && multiple.value().affected_rows == 2 &&
                 multiple.value().rids.size() == 2,
             "多行 INSERT 应在同一语句中全部写入")) {
    return false;
  }

  auto rows = engine.ExecuteSql("SELECT * FROM student;");
  if (!Check(rows.ok() && rows.value().rows.size() == 4 &&
                   rows.value().rows[1][0].AsInt() == 2 &&
                   rows.value().rows[1][1].AsVarchar() == "Bob" &&
                   rows.value().rows[3][0].AsInt() == 3,
               "指定列和多行 INSERT 结果应可查询")) return false;

  if (!Check(engine.ExecuteSql(
                 "CREATE TABLE optional_values(id INT, note VARCHAR DEFAULT 'new');"
                 "INSERT INTO optional_values(id) VALUES(7);").ok(),
             "指定列 INSERT 应填充 DEFAULT")) return false;
  auto defaulted = engine.ExecuteSql("SELECT note FROM optional_values;");
  if (!Check(defaulted.ok() &&
                 defaulted.value().rows[0][0].AsVarchar() == "new",
             "省略列应读到默认值")) return false;

  if (!Check(engine.ExecuteSql(
                 "CREATE TABLE atomic_rows(id INT, value VARCHAR);"
                 "CREATE UNIQUE INDEX idx_atomic_id ON atomic_rows(id);").ok(),
             "批量原子性测试表应创建")) return false;
  auto rejected = engine.ExecuteSql(
      "INSERT INTO atomic_rows VALUES(1, 'first'), (1, 'duplicate');");
  auto empty = engine.ExecuteSql("SELECT * FROM atomic_rows;");
  return Check(!rejected.ok() && empty.ok() && empty.value().rows.empty(),
               "多行 INSERT 任一行失败时应回滚整条语句");
}

bool TestDistinctAndLimitExecution() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "DISTINCT/LIMIT 测试数据库打开应成功")) {
    return false;
  }
  auto setup = engine.ExecuteSql(
      "CREATE TABLE student(id INT, name VARCHAR);"
      "INSERT INTO student VALUES(1, 'Alice');"
      "INSERT INTO student VALUES(2, 'Alice');"
      "INSERT INTO student VALUES(3, 'Bob');");
  if (!Check(setup.ok(), "DISTINCT/LIMIT 测试初始化应成功")) return false;

  auto distinct = engine.ExecuteSql("SELECT DISTINCT name FROM student;");
  if (!Check(distinct.ok() && distinct.value().rows.size() == 2 &&
                 distinct.value().rows[0][0].AsVarchar() == "Alice" &&
                 distinct.value().rows[1][0].AsVarchar() == "Bob",
             "DISTINCT 应按投影结果去重并保留首次出现顺序")) {
    return false;
  }

  auto limit = engine.ExecuteSql("SELECT * FROM student LIMIT 1;");
  return Check(limit.ok() && limit.value().rows.size() == 1 &&
                   limit.value().rows[0][0].AsInt() == 1,
               "LIMIT 应截断输出行");
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
  if (!Check(column_alias.ok() && column_alias.value().column_names.size() == 1 &&
                 column_alias.value().column_names[0] == "user_id",
             "列别名应用于结果列命名")) {
    return false;
  }

  auto table_alias =
      engine.ExecuteSql("SELECT s.id FROM student AS s;");
  if (!Check(table_alias.ok() && table_alias.value().rows.size() == 1,
             "表别名应使用 Planner 已完成的名称绑定执行")) {
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

bool TestAggregateAndHavingExecution() {
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
  if (!Check(aggregate.ok() && aggregate.value().rows.size() == 1 &&
                 aggregate.value().rows[0][0].AsInt() == 2,
             "COUNT(*) 应返回输入行数")) {
    return false;
  }

  auto all_aggregates = engine.ExecuteSql(
      "SELECT SUM(id), AVG(id), MAX(id), MIN(id) FROM student;");
  if (!Check(all_aggregates.ok() && all_aggregates.value().rows.size() == 1 &&
                 all_aggregates.value().rows[0][0].AsInt() == 3 &&
                 all_aggregates.value().rows[0][1].AsInt() == 1 &&
                 all_aggregates.value().rows[0][2].AsInt() == 2 &&
                 all_aggregates.value().rows[0][3].AsInt() == 1,
             "SUM/AVG/MAX/MIN 应计算正确")) {
    return false;
  }

  auto having = engine.ExecuteSql(
      "SELECT name, COUNT(*) FROM student "
      "GROUP BY name HAVING COUNT(*) >= 2;");
  if (!Check(having.ok() && having.value().rows.size() == 1 &&
                 having.value().rows[0][0].AsVarchar() == "Alice" &&
                 having.value().rows[0][1].AsInt() == 2,
             "聚合 HAVING 应在分组聚合后过滤")) {
    return false;
  }

  auto rows = engine.ExecuteSql("SELECT * FROM student;");
  return Check(rows.ok() && rows.value().rows.size() == 2,
               "聚合查询不得影响原始数据");
}

bool TestComplexWhereAndJoinExecution() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 4);
  if (!Check(engine.GetInitStatus().ok(), "WHERE/JOIN 测试数据库应打开")) return false;
  auto setup = engine.ExecuteSql(
      "CREATE TABLE users(id INT, name VARCHAR(16));"
      "CREATE TABLE scores(user_id INT, score INT);"
      "INSERT INTO users VALUES(1, 'Alice');"
      "INSERT INTO users VALUES(2, 'Bob');"
      "INSERT INTO users VALUES(3, 'Cara');"
      "INSERT INTO scores VALUES(1, 80);"
      "INSERT INTO scores VALUES(3, 95);");
  if (!Check(setup.ok(), "WHERE/JOIN 测试数据应创建")) return false;

  auto filtered = engine.ExecuteSql(
      "SELECT id FROM users WHERE (id >= 2 AND name LIKE 'C%') "
      "OR id BETWEEN 1 AND 1 ORDER BY id;");
  if (!Check(filtered.ok() && filtered.value().rows.size() == 2 &&
                 filtered.value().rows[0][0].AsInt() == 1 &&
                 filtered.value().rows[1][0].AsInt() == 3,
             "WHERE 应执行比较、AND/OR、BETWEEN 和 LIKE")) return false;

  auto in_not = engine.ExecuteSql(
      "SELECT id FROM users WHERE id IN (1, 3) AND NOT name = 'Alice';");
  if (!Check(in_not.ok() && in_not.value().rows.size() == 1 &&
                 in_not.value().rows[0][0].AsInt() == 3,
             "WHERE 应执行 IN 和 NOT")) return false;

  auto joined = engine.ExecuteSql(
      "SELECT u.name AS student_name, s.score FROM users AS u "
      "INNER JOIN scores AS s ON u.id = s.user_id;");
  return Check(joined.ok() && joined.value().rows.size() == 2 &&
                   joined.value().column_names[0] == "student_name" &&
                   joined.value().rows[0][0].AsVarchar() == "Alice" &&
                   joined.value().rows[0][1].AsInt() == 80 &&
                   joined.value().rows[1][0].AsVarchar() == "Cara" &&
                   joined.value().rows[1][1].AsInt() == 95,
               "INNER JOIN 应执行等值连接、表别名和输出别名");
}

bool TestOuterJoinSubqueryAndAlterExecution() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 5);
    if (!Check(engine.GetInitStatus().ok(), "扩展执行数据库应打开")) return false;
    auto setup = engine.ExecuteSql(
        "CREATE TABLE users(id INT, name VARCHAR(16));"
        "CREATE TABLE scores(user_id INT, score INT);"
        "INSERT INTO users VALUES(1, 'Alice'), (2, 'Bob');"
        "INSERT INTO scores VALUES(1, 80), (3, 95);"
        "CREATE UNIQUE INDEX idx_users_id ON users(id);");
    if (!Check(setup.ok(), "外连接/子查询/ALTER 数据应创建")) return false;

    auto left = engine.ExecuteSql(
        "SELECT u.id, s.score FROM users AS u LEFT JOIN scores AS s "
        "ON u.id = s.user_id;");
    if (!Check(left.ok() && left.value().rows.size() == 2 &&
                   left.value().rows[1][0].AsInt() == 2 &&
                   left.value().rows[1][1].IsNull(),
               "LEFT JOIN 应保留左侧未匹配行")) return false;

    auto right = engine.ExecuteSql(
        "SELECT u.name, s.score FROM users AS u RIGHT JOIN scores AS s "
        "ON u.id = s.user_id;");
    if (!Check(right.ok() && right.value().rows.size() == 2 &&
                   right.value().rows[1][0].IsNull() &&
                   right.value().rows[1][1].AsInt() == 95,
               "RIGHT JOIN 应保留右侧未匹配行")) return false;

    auto full = engine.ExecuteSql(
        "SELECT u.id, s.user_id FROM users AS u FULL JOIN scores AS s "
        "ON u.id = s.user_id;");
    if (!Check(full.ok() && full.value().rows.size() == 3,
               "FULL JOIN 应合并两侧未匹配行")) return false;

    auto in_query = engine.ExecuteSql(
        "SELECT id FROM users WHERE id IN "
        "(SELECT user_id FROM scores WHERE score >= 90);");
    if (!Check(in_query.ok() && in_query.value().rows.empty(),
               "IN 子查询应执行并过滤外层行")) return false;
    auto exists = engine.ExecuteSql(
        "SELECT id FROM users WHERE EXISTS "
        "(SELECT user_id FROM scores WHERE score = 80);");
    if (!Check(exists.ok() && exists.value().rows.size() == 2,
               "EXISTS 子查询应按结果是否为空求值")) return false;
    auto scalar = engine.ExecuteSql(
        "SELECT (SELECT score FROM scores WHERE user_id = 1) AS best "
        "FROM users LIMIT 1;");
    if (!Check(scalar.ok() && scalar.value().rows.size() == 1 &&
                   scalar.value().column_names[0] == "best" &&
                   scalar.value().rows[0][0].AsInt() == 80,
               "标量子查询应可用于 SELECT 列表")) return false;

    if (!Check(engine.ExecuteSql(
                   "ALTER TABLE users RENAME COLUMN id TO user_id;").ok() &&
                   engine.ExecuteSql(
                   "ALTER TABLE users RENAME COLUMN name TO display_name;").ok(),
               "ALTER RENAME COLUMN 应执行")) return false;
    if (!Check(engine.ExecuteSql(
                   "ALTER TABLE users ADD COLUMN age INT DEFAULT 18;").ok(),
               "ALTER ADD COLUMN 应执行")) return false;
    if (!Check(engine.ExecuteSql("ALTER TABLE users RENAME TO accounts;").ok(),
               "ALTER RENAME TABLE 应执行")) return false;
    auto altered = engine.ExecuteSql(
        "SELECT user_id, display_name, age FROM accounts ORDER BY user_id;");
    if (!Check(altered.ok() && altered.value().rows.size() == 2 &&
                   altered.value().rows[0][0].AsInt() == 1 &&
                   altered.value().rows[0][1].AsVarchar() == "Alice" &&
                   altered.value().rows[0][2].AsInt() == 18,
               "ALTER 应保留原行并填充新列默认值")) return false;
    auto indexed = engine.ExecuteSql("SELECT * FROM accounts WHERE user_id = 2;");
    if (!Check(indexed.ok() && indexed.value().rows.size() == 1,
               "ALTER 后重建的索引应可查询")) return false;
    if (!Check(engine.Close().ok(), "扩展执行数据库应关闭")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 4);
  auto inserted_after_restart = reopened.ExecuteSql(
      "INSERT INTO accounts(user_id, display_name) VALUES(3, 'Cara');");
  auto persisted = reopened.ExecuteSql(
      "SELECT user_id, display_name, age FROM accounts ORDER BY user_id;");
  return Check(reopened.GetInitStatus().ok() && inserted_after_restart.ok() &&
                   persisted.ok() && persisted.value().rows.size() == 3 &&
                   persisted.value().rows[1][1].AsVarchar() == "Bob" &&
                   persisted.value().rows[1][2].AsInt() == 18 &&
                   persisted.value().rows[2][1].AsVarchar() == "Cara" &&
                   persisted.value().rows[2][2].AsInt() == 18 &&
                   reopened.Close().ok(),
               "ALTER 后的表结构、默认值、数据和索引应持久化");
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
  auto explain = reopened.ExecuteSql("EXPLAIN SELECT * FROM student WHERE group_id = 9999;");
  return Check(explain.ok() && explain.value().rows[0][0].AsVarchar().find(
                                   "idx_student_group_rebuilt") != std::string::npos,
               "最终重启应恢复DROP后重建的索引元数据");
}

bool TestCostBasedSelectionAndIndexedDelete() {
  TempDb skew;
  {
    oursql::DatabaseEngine engine(skew.path, 8);
    if (!Check(engine.GetInitStatus().ok() &&
                   engine.ExecuteSql(
                       "CREATE TABLE people(id INT, gender INT);")
                       .ok(),
               "成本选择测试建表应成功")) {
      return false;
    }
    std::string insert = "INSERT INTO people VALUES ";
    for (int id = 0; id < 100; ++id) {
      if (id != 0) insert += ',';
      insert += '(' + std::to_string(id) + ',' +
                std::to_string(id < 90 ? 1 : 0) + ')';
    }
    insert += ';';
    if (!Check(engine.ExecuteSql(insert).ok() &&
                   engine.ExecuteSql(
                       "CREATE INDEX idx_people_gender ON people(gender);")
                       .ok(),
               "成本选择测试数据和索引应建立")) {
      return false;
    }
    auto common = engine.ExecuteSql(
        "EXPLAIN SELECT * FROM people WHERE gender = 1;");
    auto rare = engine.ExecuteSql(
        "EXPLAIN SELECT * FROM people WHERE gender = 0;");
    if (!Check(common.ok() && rare.ok() &&
                   common.value().rows[0][0].AsVarchar().find(
                       "SeqScanPlan") != std::string::npos &&
                   rare.value().rows[0][0].AsVarchar().find(
                       "IndexScanPlan") != std::string::npos,
               "90%高频值应选SeqScan，10%低频值应选IndexScan")) {
      return false;
    }
    auto common_rows =
        engine.ExecuteSql("SELECT * FROM people WHERE gender = 1;");
    auto rare_rows =
        engine.ExecuteSql("SELECT * FROM people WHERE gender = 0;");
    if (!Check(common_rows.ok() && common_rows.value().rows.size() == 90 &&
                   rare_rows.ok() && rare_rows.value().rows.size() == 10,
               "两种成本路径必须返回相同语义下的正确结果")) {
      return false;
    }
    auto removed =
        engine.ExecuteSql("DELETE FROM people WHERE gender = 0;");
    auto after =
        engine.ExecuteSql("SELECT * FROM people WHERE gender = 0;");
    if (!Check(removed.ok() && removed.value().affected_rows == 10 &&
                   after.ok() && after.value().rows.empty(),
               "非唯一索引DELETE应按全部候选RID删除并保持索引一致")) {
      return false;
    }
  }

  TempDb delete_probe;
  {
    oursql::DatabaseEngine engine(delete_probe.path, 8);
    if (!Check(engine.GetInitStatus().ok() &&
                   engine.ExecuteSql(
                       "CREATE TABLE probe(id INT, payload VARCHAR(256));")
                       .ok(),
               "索引DELETE访问量测试建表应成功")) {
      return false;
    }
    const std::string payload(180, 'x');
    for (int first = 0; first < 800; first += 100) {
      std::string batch;
      for (int id = first; id < first + 100; ++id) {
        batch += "INSERT INTO probe VALUES(" + std::to_string(id) +
                 ",'" + payload + "');";
      }
      if (!Check(engine.ExecuteSqlBatch(batch).ok(),
                 "索引DELETE访问量测试批量插入应成功")) {
        return false;
      }
    }
    if (!Check(engine.ExecuteSql(
                   "CREATE UNIQUE INDEX idx_probe_id ON probe(id);")
                   .ok() &&
                   engine.Close().ok(),
               "索引DELETE访问量测试索引和持久化应成功")) {
      return false;
    }
  }
  oursql::DatabaseEngine reopened(delete_probe.path, 8);
  if (!Check(reopened.GetInitStatus().ok() &&
                 reopened.ResetStatistics().ok(),
             "索引DELETE访问量测试重启应成功")) {
    return false;
  }
  auto full_scan = reopened.ExecuteSql("SELECT * FROM probe;");
  const auto full_scan_accesses = reopened.GetStatistics().buffer_accesses;
  if (!Check(full_scan.ok() && full_scan.value().rows.size() == 800 &&
                 reopened.ResetStatistics().ok(),
             "索引DELETE访问量测试全表扫描基线应成功")) {
    return false;
  }
  auto indexed_delete = reopened.ExecuteSql("DELETE FROM probe WHERE id = 0;");
  const auto indexed_delete_accesses =
      reopened.GetStatistics().buffer_accesses;
  return Check(indexed_delete.ok() &&
                   indexed_delete.value().affected_rows == 1 &&
                   indexed_delete_accesses * 2 < full_scan_accesses,
               "索引DELETE访问页次数应显著少于全表扫描");
}

bool TestFailedTransactionState() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(), "Failed transaction test engine should open")) {
    return false;
  }
  if (!Check(engine.ExecuteSql("CREATE TABLE student(id INT, name VARCHAR);").ok(),
             "Failed transaction test table setup")) {
    return false;
  }
  if (!Check(engine.ExecuteSql("BEGIN;").ok(), "Failed transaction test begin")) return false;

  auto parser_error = engine.ExecuteSql("SELECT FROM student;");
  if (!Check(!parser_error.ok(), "parser error should be reported inside transaction")) {
    return false;
  }
  auto select_while_failed = engine.ExecuteSql("SELECT * FROM student;");
  auto insert_while_failed = engine.ExecuteSql("INSERT INTO student VALUES(1, 'blocked');");
  auto commit_while_failed = engine.ExecuteSql("COMMIT;");
  auto begin_while_failed = engine.ExecuteSql("BEGIN;");
  if (!Check(!select_while_failed.ok() && !insert_while_failed.ok() &&
                 !commit_while_failed.ok() && !begin_while_failed.ok(),
             "Failed transaction must reject work, COMMIT, and BEGIN")) {
    return false;
  }
  if (!Check(engine.ExecuteSql("ROLLBACK;").ok(),
             "Failed transaction must allow ROLLBACK")) {
    return false;
  }
  auto after_rollback = engine.ExecuteSql("SELECT * FROM student;");
  if (!Check(after_rollback.ok() && after_rollback.value().rows.empty(),
             "normal SQL should resume after ROLLBACK")) {
    return false;
  }

  if (!Check(engine.ExecuteSql("BEGIN;").ok(), "planner failure transaction begin")) return false;
  auto planner_error = engine.ExecuteSql("SELECT * FROM missing;");
  if (!Check(!planner_error.ok(), "planner error should be reported inside transaction")) {
    return false;
  }
  auto blocked_after_planner_error = engine.ExecuteSql("SELECT * FROM student;");
  if (!Check(!blocked_after_planner_error.ok(),
             "planner failure must put transaction into Failed state")) {
    return false;
  }
  return Check(engine.ExecuteSql("ROLLBACK;").ok(),
               "planner-failed transaction must be recoverable by ROLLBACK") &&
         Check(engine.Close().ok(), "Failed transaction test close");
}

bool TestHeapAndIndexTransactions() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 3);
    if (!Check(engine.GetInitStatus().ok(), "Heap/index transaction engine should open")) {
      return false;
    }
    if (!Check(engine.ExecuteSql(
                   "CREATE TABLE student(id INT, name VARCHAR(16));"
                   "INSERT INTO student VALUES(1, 'Alice');"
                   "INSERT INTO student VALUES(2, 'Bob');"
                   "CREATE UNIQUE INDEX idx_student_id ON student(id);")
                   .ok(),
               "Heap/index transaction setup")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql("INSERT INTO student VALUES(10, 'rolled_back');").ok() &&
                   engine.ExecuteSql("ROLLBACK;").ok(),
               "indexed INSERT rollback")) {
      return false;
    }
    auto rolled_back_insert = engine.ExecuteSql("SELECT * FROM student WHERE id = 10;");
    if (!Check(rolled_back_insert.ok() && rolled_back_insert.value().rows.empty(),
               "rolled-back indexed INSERT must disappear from Heap and index")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql("UPDATE student SET id = 20 WHERE id = 1;").ok() &&
                   engine.ExecuteSql("ROLLBACK;").ok(),
               "indexed UPDATE rollback")) {
      return false;
    }
    auto old_key = engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
    auto new_key = engine.ExecuteSql("SELECT * FROM student WHERE id = 20;");
    if (!Check(old_key.ok() && old_key.value().rows.size() == 1 &&
                   new_key.ok() && new_key.value().rows.empty(),
               "rolled-back indexed UPDATE must restore old key and row")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql(
                       "UPDATE student SET name = 'Temporary' WHERE id = 1;")
                       .ok() &&
                   engine.ExecuteSql("ROLLBACK;").ok(),
               "same-key new-RID indexed UPDATE rollback")) {
      return false;
    }
    auto same_key_restored =
        engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
    if (!Check(same_key_restored.ok() &&
                   same_key_restored.value().rows.size() == 1 &&
                   same_key_restored.value().rows[0][1].AsVarchar() == "Alice",
               "rollback must restore an indexed row when only its RID changed")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql("DELETE FROM student WHERE id = 2;").ok() &&
                   engine.ExecuteSql("ROLLBACK;").ok(),
               "indexed DELETE rollback")) {
      return false;
    }
    auto restored_delete = engine.ExecuteSql("SELECT * FROM student WHERE id = 2;");
    if (!Check(restored_delete.ok() && restored_delete.value().rows.size() == 1,
               "rolled-back indexed DELETE must restore Heap and index entry")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok(), "index failure transaction begin")) {
      return false;
    }
    auto index_failure =
        engine.ExecuteSql("INSERT INTO student VALUES(1, 'duplicate');");
    if (!Check(!index_failure.ok() &&
                   index_failure.status().code() ==
                       oursql::ErrorCode::AlreadyExists,
               "Heap success followed by index failure must abort statement")) {
      return false;
    }
    auto blocked_after_index_failure =
        engine.ExecuteSql("SELECT * FROM student;");
    if (!Check(!blocked_after_index_failure.ok(),
               "Failed transaction after index error must reject normal SQL")) {
      return false;
    }
    if (!Check(engine.ExecuteSql("ROLLBACK;").ok(),
               "index failure transaction rollback")) {
      return false;
    }
    auto after_index_failure =
        engine.ExecuteSql("SELECT * FROM student WHERE id = 1;");
    if (!Check(after_index_failure.ok() &&
                   after_index_failure.value().rows.size() == 1,
               "rollback after index failure must keep Heap and index consistent")) {
      return false;
    }

    if (!Check(engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql("INSERT INTO student VALUES(30, 'committed');").ok() &&
                   engine.ExecuteSql("COMMIT;").ok(),
               "indexed INSERT commit")) {
      return false;
    }
    auto committed = engine.ExecuteSql("SELECT * FROM student WHERE id = 30;");
    if (!Check(committed.ok() && committed.value().rows.size() == 1,
               "committed indexed INSERT must be visible")) {
      return false;
    }
    if (!Check(engine.Close().ok(), "Heap/index transaction first close")) return false;
  }

  oursql::DatabaseEngine reopened(temp.path, 3);
  auto one = reopened.ExecuteSql("SELECT * FROM student WHERE id = 1;");
  auto two = reopened.ExecuteSql("SELECT * FROM student WHERE id = 2;");
  auto ten = reopened.ExecuteSql("SELECT * FROM student WHERE id = 10;");
  auto thirty = reopened.ExecuteSql("SELECT * FROM student WHERE id = 30;");
  const bool ok = Check(reopened.GetInitStatus().ok(), "Heap/index transaction restart") &&
                  Check(one.ok() && one.value().rows.size() == 1 &&
                            two.ok() && two.value().rows.size() == 1 &&
                            ten.ok() && ten.value().rows.empty() &&
                            thirty.ok() && thirty.value().rows.size() == 1,
                        "Heap and index must agree after restart");
  return reopened.Close().ok() && ok;
}

bool TestCloseRollsBackActiveTransaction() {
  TempDb temp;
  {
    oursql::DatabaseEngine engine(temp.path, 3);
    if (!Check(engine.GetInitStatus().ok(),
               "Close rollback test database should open")) {
      return false;
    }
    if (!Check(engine.ExecuteSql(
                   "CREATE TABLE student(id INT, name VARCHAR);")
                   .ok() &&
                   engine.ExecuteSql("BEGIN;").ok() &&
                   engine.ExecuteSql(
                       "INSERT INTO student VALUES(1, 'Uncommitted');")
                       .ok(),
               "Close rollback test setup")) {
      return false;
    }
    if (!Check(engine.Close().ok(),
               "Close should automatically roll back active transaction")) {
      return false;
    }
  }

  oursql::DatabaseEngine reopened(temp.path, 3);
  if (!Check(reopened.GetInitStatus().ok(),
             "Close rollback database should reopen")) {
    return false;
  }
  auto rows = reopened.ExecuteSql("SELECT * FROM student;");
  return Check(rows.ok() && rows.value().rows.empty(),
               "Close rollback must remove uncommitted row") &&
         Check(reopened.Close().ok(), "Close rollback final close");
}

bool TestTransactionCommandStateErrors() {
  TempDb temp;
  oursql::DatabaseEngine engine(temp.path, 3);
  if (!Check(engine.GetInitStatus().ok(),
             "transaction state-error database should open")) {
    return false;
  }

  auto commit_without_begin = engine.ExecuteSql("COMMIT;");
  auto rollback_without_begin = engine.ExecuteSql("ROLLBACK;");
  if (!Check(!commit_without_begin.ok() &&
                 commit_without_begin.status().code() ==
                     oursql::ErrorCode::NotFound &&
                 !rollback_without_begin.ok() &&
                 rollback_without_begin.status().code() ==
                     oursql::ErrorCode::NotFound,
             "COMMIT/ROLLBACK without BEGIN must report no active transaction")) {
    return false;
  }

  auto begin = engine.ExecuteSql("BEGIN;");
  if (!Check(begin.ok() && begin.value().column_names ==
                                  std::vector<std::string>{"status"} &&
                 begin.value().rows.size() == 1 &&
                 begin.value().rows[0][0].AsVarchar().find(
                     "Transaction started") == 0,
             "BEGIN must report transaction started")) {
    return false;
  }
  auto nested_begin = engine.ExecuteSql("BEGIN;");
  if (!Check(!nested_begin.ok() &&
                 nested_begin.status().code() ==
                     oursql::ErrorCode::AlreadyExists,
             "nested BEGIN must be rejected")) {
    return false;
  }

  auto commit = engine.ExecuteSql("COMMIT;");
  if (!Check(commit.ok() && commit.value().rows.size() == 1 &&
                 commit.value().rows[0][0].AsVarchar() ==
                     "Transaction committed",
             "COMMIT must report transaction committed")) {
    return false;
  }
  auto commit_again = engine.ExecuteSql("COMMIT;");
  return Check(!commit_again.ok() &&
                   commit_again.status().code() ==
                       oursql::ErrorCode::NotFound &&
                   engine.Close().ok(),
               "COMMIT after completed transaction must report no active transaction");
}

}  // namespace

int main() {
  if (TestMinimumChainAndRestart()) {
    std::cout << "[PASS] Minimum execution chain and restart\n";
  } else {
    return 1;
  }
  if (TestWebFacingDatabaseContracts()) {
    std::cout << "[PASS] Web-facing database contracts\n";
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
  if (TestExtendedInsertExecution()) {
    std::cout << "[PASS] Extended INSERT execution\n";
  } else {
    return 1;
  }
  if (TestDistinctAndLimitExecution()) {
    std::cout << "[PASS] DISTINCT and LIMIT execution\n";
  } else {
    return 1;
  }
  if (TestDropTableExecutionAndAliases()) {
    std::cout << "[PASS] DROP TABLE execution and aliases\n";
  } else {
    return 1;
  }
  if (TestAggregateAndHavingExecution()) {
    std::cout << "[PASS] Aggregate and HAVING execution\n";
  } else {
    return 1;
  }
  if (TestComplexWhereAndJoinExecution()) {
    std::cout << "[PASS] Complex WHERE and JOIN execution\n";
  } else {
    return 1;
  }
  if (TestOuterJoinSubqueryAndAlterExecution()) {
    std::cout << "[PASS] Outer JOIN, subquery and ALTER execution\n";
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
  if (TestCostBasedSelectionAndIndexedDelete()) {
    std::cout << "[PASS] Cost-based selection and indexed DELETE\n";
  } else {
    return 1;
  }
  if (TestFailedTransactionState()) {
    std::cout << "[PASS] Failed transaction state\n";
  } else {
    return 1;
  }
  if (TestHeapAndIndexTransactions()) {
    std::cout << "[PASS] Heap and index transactions\n";
  } else {
    return 1;
  }
  if (TestCloseRollsBackActiveTransaction()) {
    std::cout << "[PASS] Close rolls back active transaction\n";
  } else {
    return 1;
  }
  if (TestTransactionCommandStateErrors()) {
    std::cout << "[PASS] Transaction command state errors\n";
  } else {
    return 1;
  }
  if (!TestOrderGroupAndUpdate()) return 1;
  std::cout << "[PASS] ORDER BY, GROUP BY and UPDATE execution\n";
  return 0;
}
