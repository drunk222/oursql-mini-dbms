#include "oursql/ast/statement.h"
#include "oursql/catalog/catalog.h"
#include "oursql/parser/lexer.h"
#include "oursql/parser/parser.h"
#include "oursql/plan/plan.h"
#include "oursql/planner/planner.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

static_assert(std::is_same_v<decltype(std::declval<oursql::SelectPlan>().root),
                             std::shared_ptr<const oursql::PlanNode>>);
static_assert(std::is_same_v<decltype(std::declval<oursql::FilterPlan>().child),
                             std::shared_ptr<const oursql::PlanNode>>);
static_assert(std::is_same_v<decltype(std::declval<oursql::ProjectPlan>().child),
                             std::shared_ptr<const oursql::PlanNode>>);

bool Check(bool condition, const std::string &message) {
  if (!condition) std::cerr << "[FAIL] " << message << '\n';
  return condition;
}

bool TestLexer() {
  oursql::Lexer lexer;
  auto result = lexer.Tokenize("SELECT id, name FROM t WHERE id = 1;");
  if (!Check(result.ok(), "Lexer 应成功处理 SELECT")) return false;
  const auto &tokens = result.value();
  const std::vector<oursql::TokenType> expected{
      oursql::TokenType::Select, oursql::TokenType::Identifier, oursql::TokenType::Comma,
      oursql::TokenType::Identifier, oursql::TokenType::From, oursql::TokenType::Identifier,
      oursql::TokenType::Where, oursql::TokenType::Identifier, oursql::TokenType::Equal,
      oursql::TokenType::Integer, oursql::TokenType::Semicolon, oursql::TokenType::EndOfFile};
  if (!Check(tokens.size() == expected.size(), "Token 数量应稳定")) return false;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!Check(tokens[i].type == expected[i], "Token 类型应符合语法")) return false;
  }
  return Check(tokens[1].lexeme == "id" && tokens[1].line == 1 && tokens[1].column == 8,
               "标识符应规范化并记录起始位置");
}

bool TestLexerCommentsAndNumberErrors() {
  oursql::Lexer lexer;
  auto result = lexer.Tokenize(
      "SELECT id /* column comment */ FROM users -- line comment\n"
      "WHERE id = 1;");
  if (!Check(result.ok(), "注释不应阻止 Lexer 成功")) return false;
  bool has_select = false;
  bool has_where = false;
  for (const auto &token : result.value()) {
    if (token.type == oursql::TokenType::Select) has_select = true;
    if (token.type == oursql::TokenType::Where) has_where = true;
  }
  if (!Check(has_select && has_where, "注释内容不应生成 Token")) return false;

  auto bad_number = lexer.Tokenize("SELECT 1.2;");
  if (!Check(!bad_number.ok() &&
                 bad_number.status().message().find("非法数字") != std::string::npos,
             "非法数字应返回专门错误")) {
    return false;
  }
  auto unclosed = lexer.Tokenize("SELECT 1; /* no close");
  return Check(!unclosed.ok() &&
                   unclosed.status().message().find("未闭合块注释") != std::string::npos,
               "未闭合块注释应报错");
}

bool TestParserAndCaseInsensitiveMultiStatement() {
  oursql::Parser parser;
  auto result = parser.Parse(
      "CrEaTe TaBlE Users(ID INT, Name VARCHAR); INSERT INTO USERS VALUES(1, 'Alice'); "
      "SELECT ID, Name FROM users WHERE ID = 1 GROUP BY ID, Name ORDER BY ID DESC; "
      "DELETE FROM users WHERE id = 1;");
  if (!Check(result.ok() && result.value().size() == 4, "大小写不敏感多语句应解析成功")) return false;

  const auto &create = std::get<oursql::CreateTableStatement>(result.value()[0]);
  const auto &insert = std::get<oursql::InsertStatement>(result.value()[1]);
  const auto &select = std::get<oursql::SelectStatement>(result.value()[2]);
  const auto &delete_statement = std::get<oursql::DeleteStatement>(result.value()[3]);
  const bool fields = Check(create.table_name == "users" && create.schema.size() == 2,
                             "CREATE AST 核心字段应正确") &&
                      Check(create.schema.At(0).name == "id" && create.schema.At(0).type == oursql::DataType::Int,
                            "CREATE INT 列应正确") &&
                      Check(insert.table_name == "users" &&
                                insert.columns.empty() &&
                                insert.rows.size() == 1 &&
                                insert.rows[0].size() == 2 &&
                                insert.rows[0][1].AsVarchar() == "Alice",
                            "INSERT AST 核心字段应正确") &&
                      Check(select.table_name == "users" && !select.select_all &&
                                select.projection == std::vector<std::string>{"id", "name"} &&
                                select.where.has_value() && select.where->value.AsInt() == 1,
                            "SELECT AST 核心字段应正确") &&
                      Check(select.group_by == std::vector<std::string>{"id", "name"} &&
                                select.order_by.size() == 1 &&
                                select.order_by[0].column == "id" &&
                                select.order_by[0].direction ==
                                    oursql::OrderDirection::Desc,
                            "GROUP BY/ORDER BY AST 字段应正确") &&
                      Check(select.location.line > 0 && select.location.column > 0,
                            "SELECT AST 应记录起始位置") &&
                      Check(delete_statement.table_name == "users" && delete_statement.where.has_value(),
                            "DELETE AST 核心字段应正确");
  if (!fields) return false;
  return Check(oursql::ToString(result.value()[2]) ==
                   "SelectStatement(table=users,projection=[id,name],where=id=1,"
                   "group_by=[id,name],order_by=[id DESC])",
               "AST 文本格式应稳定");
}

bool TestParserErrorsHavePositions() {
  oursql::Parser parser;
  const std::vector<std::string> invalid_sql{
      "CREATE TABLE t(id INT name VARCHAR);",
      "CREATE TABLE t(id INT;",
      "SELECT id FROM t",
      "UPSERT INTO t VALUES(1);",
      "INSERT INTO t (id name) VALUES(1, 'Alice');",
      "INSERT INTO t (id, name) VALUES 1, 'Alice';",
  };
  for (const auto &sql : invalid_sql) {
    auto result = parser.Parse(sql);
    if (!Check(!result.ok(), "非法 SQL 应返回错误")) return false;
    if (!Check(result.status().message().find("位置") != std::string::npos,
               "Parser 错误应包含位置")) return false;
  }
  auto unterminated = parser.Parse("INSERT INTO t VALUES('Alice);");
  return Check(!unterminated.ok(), "未闭合字符串应返回错误") &&
         Check(unterminated.status().message().find("1:22") != std::string::npos,
               "未闭合字符串应包含起始位置");
}

oursql::Catalog MakeCatalog() {
  oursql::Catalog catalog;
  (void)catalog.CreateTable(oursql::TableInfo{
      "users", oursql::Schema{{"id", oursql::DataType::Int}, {"name", oursql::DataType::Varchar, 5}}});
  return catalog;
}

class FixedCatalogReader final : public oursql::CatalogView {
 public:
  FixedCatalogReader(oursql::TableInfo table,
                     std::vector<oursql::IndexMetadata> indexes)
      : table_(std::move(table)), indexes_(std::move(indexes)) {}

  oursql::Result<const oursql::TableInfo *> FindTable(
      std::string_view table_name) const override {
    if (table_name != table_.name) {
      return oursql::Result<const oursql::TableInfo *>(
          oursql::Status::NotFound("测试 Catalog 未找到表"));
    }
    return oursql::Result<const oursql::TableInfo *>(&table_);
  }

  std::vector<oursql::IndexMetadata> ListTableIndexes(
      std::string_view table_name) const override {
    if (table_name != table_.name) return {};
    return indexes_;
  }

 private:
  oursql::TableInfo table_;
  std::vector<oursql::IndexMetadata> indexes_;
};

bool TestPlannerSemanticChecks() {
  oursql::Parser parser;
  auto table = parser.Parse("CREATE TABLE fresh(id INT, name VARCHAR);");
  if (!Check(table.ok(), "CREATE 应可解析")) return false;
  oursql::Catalog empty;
  oursql::Planner planner;
  auto create_plan = planner.Build(table.value()[0], empty);
  if (!Check(create_plan.ok() && std::holds_alternative<oursql::CreateTablePlan>(create_plan.value()),
             "CREATE 应生成 CreateTablePlan")) return false;

  auto catalog = MakeCatalog();
  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"INSERT INTO missing VALUES(1, 'a');", oursql::ErrorCode::NotFound},
      {"SELECT unknown FROM users;", oursql::ErrorCode::NotFound},
      {"SELECT id FROM users ORDER BY missing;", oursql::ErrorCode::NotFound},
      {"SELECT id FROM users GROUP BY missing;", oursql::ErrorCode::NotFound},
      {"SELECT id, id FROM users;", oursql::ErrorCode::AlreadyExists},
      {"CREATE TABLE duplicate(id INT, id VARCHAR);", oursql::ErrorCode::AlreadyExists},
      {"INSERT INTO users VALUES(1);", oursql::ErrorCode::InvalidArgument},
      {"INSERT INTO users VALUES('1', 'Alice');", oursql::ErrorCode::TypeMismatch},
      {"SELECT id FROM users WHERE id = '1';", oursql::ErrorCode::TypeMismatch},
      {"INSERT INTO users VALUES(1, 'toolong');", oursql::ErrorCode::InvalidArgument},
  };
  for (const auto &item : invalid) {
    auto statements = parser.Parse(item.first);
    if (!Check(statements.ok(), "语义错误样例自身应先通过 Parser")) return false;
    auto plan = planner.Build(statements.value()[0], catalog);
    if (!Check(!plan.ok() && plan.status().code() == item.second, "语义错误类型应准确")) return false;
  }
  return true;
}

bool TestPlanTreeAndPrinting() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  auto statements = parser.Parse("SELECT id, name FROM users WHERE id = 1;");
  if (!Check(statements.ok(), "SELECT WHERE 应解析成功")) return false;
  oursql::Planner planner;
  auto plan_result = planner.Build(statements.value()[0], catalog);
  if (!Check(plan_result.ok(), "SELECT WHERE 应规划成功")) return false;
  const auto &select_plan = std::get<oursql::SelectPlan>(plan_result.value());
  if (!Check(select_plan.root != nullptr, "SELECT 计划应有根算子")) return false;
  const auto &project = std::get<oursql::ProjectPlan>(select_plan.root->operation);
  if (!Check(!project.select_all && project.columns == std::vector<std::string>{"id", "name"},
             "ProjectPlan 字段应正确")) return false;
  const auto &filter = std::get<oursql::FilterPlan>(project.child->operation);
  if (!Check(filter.predicate.column == "id" && filter.predicate.value.AsInt() == 1,
             "FilterPlan 条件应正确")) return false;
  const auto &scan = std::get<oursql::SeqScanPlan>(filter.child->operation);
  if (!Check(scan.table_name == "users", "SeqScanPlan 表名应正确")) return false;
  const auto plan_text = oursql::ToString(plan_result.value());
  if (!Check(plan_text.find("ProjectPlan") != std::string::npos &&
                 plan_text.find("FilterPlan") != std::string::npos &&
                 plan_text.find("SeqScanPlan") != std::string::npos,
             "Plan 文本应包含算子链")) {
    return false;
  }

  auto insert_statements = parser.Parse("INSERT INTO users VALUES(1, 'Alice');");
  auto insert_plan = planner.Build(insert_statements.value()[0], catalog);
  if (!Check(insert_plan.ok(), "INSERT 应规划成功")) return false;
  const auto &insert_plan_value =
      std::get<oursql::InsertPlan>(insert_plan.value());
  if (!Check(insert_plan_value.columns.empty() &&
                 insert_plan_value.rows.size() == 1 &&
                 insert_plan_value.rows[0].size() == 2,
             "INSERT 应生成正确的 InsertPlan")) {
    return false;
  }
  auto delete_statements = parser.Parse("DELETE FROM users WHERE id = 1;");
  auto delete_plan = planner.Build(delete_statements.value()[0], catalog);
  if (!Check(delete_plan.ok() && std::get<oursql::DeletePlan>(delete_plan.value()).where.has_value(),
             "DELETE 应生成正确的 DeletePlan")) {
    return false;
  }
  auto select_all_statements = parser.Parse("SELECT * FROM users;");
  auto select_all_plan = planner.Build(select_all_statements.value()[0], catalog);
  if (!Check(select_all_plan.ok(), "SELECT * 应规划成功")) return false;
  const auto &select_all = std::get<oursql::SelectPlan>(select_all_plan.value());
  const auto &project_all = std::get<oursql::ProjectPlan>(select_all.root->operation);
  return Check(project_all.select_all && project_all.columns.empty(), "SELECT * 的 ProjectPlan 应保留全列标记");
}

bool TestInsertColumnListAndMultipleRows() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;

  auto parsed = parser.Parse(
      "INSERT INTO users (name, id) VALUES ('Alice', 1), ('Bob', 2);");
  if (!Check(parsed.ok(), "指定列多行 INSERT 应解析成功")) return false;
  const auto &insert =
      std::get<oursql::InsertStatement>(parsed.value()[0]);
  if (!Check(insert.table_name == "users" &&
                 insert.columns == std::vector<std::string>{"name", "id"} &&
                 insert.rows.size() == 2 &&
                 insert.rows[0][0].AsVarchar() == "Alice" &&
                 insert.rows[0][1].AsInt() == 1 &&
                 insert.rows[1][0].AsVarchar() == "Bob" &&
                 insert.rows[1][1].AsInt() == 2,
             "指定列多行 INSERT 的 AST 字段应正确")) {
    return false;
  }

  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::InsertPlan>(plan.value()),
             "指定列多行 INSERT 应生成 InsertPlan")) {
    return false;
  }
  const auto &insert_plan = std::get<oursql::InsertPlan>(plan.value());
  if (!Check(insert_plan.columns ==
                     std::vector<std::string>{"name", "id"} &&
                 insert_plan.rows.size() == 2 &&
                 insert_plan.rows[0][0].AsInt() == 1 &&
                 insert_plan.rows[0][1].AsVarchar() == "Alice" &&
                 insert_plan.rows[1][0].AsInt() == 2 &&
                 insert_plan.rows[1][1].AsVarchar() == "Bob",
             "指定列多行 INSERT 的 Plan 字段应正确")) {
    return false;
  }

  auto full_rows = parser.Parse(
      "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob');");
  if (!Check(full_rows.ok(), "全列多行 INSERT 应解析成功")) return false;
  auto full_plan = planner.Build(full_rows.value()[0], catalog);
  if (!Check(full_plan.ok(), "全列多行 INSERT 应规划成功")) {
    return false;
  }
  const auto &full_insert_plan =
      std::get<oursql::InsertPlan>(full_plan.value());
  if (!Check(full_insert_plan.columns.empty() &&
                 full_insert_plan.rows.size() == 2,
             "未指定目标列时应保存全列顺序的多行值")) {
    return false;
  }

  auto partial = parser.Parse(
      "INSERT INTO users (name) VALUES ('Alice');");
  if (!Check(partial.ok(), "指定部分列 INSERT 应解析成功")) return false;
  auto partial_plan = planner.Build(partial.value()[0], catalog);
  if (!Check(partial_plan.ok(), "指定部分列应完成编译层语义检查")) {
    return false;
  }
  const auto &partial_insert_plan =
      std::get<oursql::InsertPlan>(partial_plan.value());
  if (!Check(partial_insert_plan.columns ==
                     std::vector<std::string>{"name"} &&
                 partial_insert_plan.rows.size() == 1 &&
                 partial_insert_plan.rows[0][0].IsNull() &&
                 partial_insert_plan.rows[0][1].AsVarchar() == "Alice",
             "部分列值应填充为 Schema 顺序并使用 NULL 默认")) {
    return false;
  }

  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"INSERT INTO users (id, id) VALUES(1, 2);",
       oursql::ErrorCode::AlreadyExists},
      {"INSERT INTO users (missing) VALUES(1);",
       oursql::ErrorCode::NotFound},
      {"INSERT INTO users (id) VALUES(1, 2);",
       oursql::ErrorCode::InvalidArgument},
      {"INSERT INTO users (id, name) VALUES('1', 'Alice');",
       oursql::ErrorCode::TypeMismatch},
      {"INSERT INTO users (name) VALUES('toolong');",
       oursql::ErrorCode::InvalidArgument},
  };
  for (const auto &item : invalid) {
    auto statements = parser.Parse(item.first);
    if (!Check(statements.ok(), "INSERT 语义错误样例应先通过 Parser")) {
      return false;
    }
    auto invalid_plan = planner.Build(statements.value()[0], catalog);
    if (!Check(!invalid_plan.ok() &&
                   invalid_plan.status().code() == item.second,
               "INSERT 指定列和多行语义错误类型应准确")) {
      return false;
    }
  }

  return Check(
      oursql::ToString(parsed.value()[0]) ==
              "InsertStatement(table=users,columns=[name,id],"
              "rows=[['Alice',1],['Bob',2]])" &&
          oursql::ToString(plan.value()) ==
              "InsertPlan(table=users,columns=[name,id],"
              "rows=[[1,'Alice'],[2,'Bob']])",
      "INSERT AST 和 Plan 文本格式应稳定");
}

bool TestDistinctAndLimitCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;

  auto parsed = parser.Parse(
      "SELECT DISTINCT name FROM users LIMIT 0;");
  if (!Check(parsed.ok(), "DISTINCT LIMIT SELECT 应解析成功")) return false;
  const auto &select =
      std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.distinct &&
                 select.limit.has_value() &&
                 *select.limit == 0,
             "DISTINCT 和 LIMIT 的 AST 字段应正确")) {
    return false;
  }

  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::SelectPlan>(plan.value()),
             "DISTINCT LIMIT SELECT 应生成 SelectPlan")) {
    return false;
  }
  const auto &select_plan = std::get<oursql::SelectPlan>(plan.value());
  if (!Check(select_plan.distinct &&
                 select_plan.limit.has_value() &&
                 *select_plan.limit == 0 &&
                 select_plan.root != nullptr,
             "DISTINCT 和 LIMIT 应保留在 SelectPlan 中")) {
    return false;
  }

  auto ordinary = parser.Parse(
      "SELECT id FROM users LIMIT 12;");
  if (!Check(ordinary.ok(), "普通 LIMIT SELECT 应解析成功")) return false;
  const auto &ordinary_select =
      std::get<oursql::SelectStatement>(ordinary.value()[0]);
  if (!Check(!ordinary_select.distinct &&
                 ordinary_select.limit.has_value() &&
                 *ordinary_select.limit == 12,
             "未使用 DISTINCT 时 AST 标记应为 false")) {
    return false;
  }

  const std::vector<std::string> invalid{
      "SELECT id FROM users LIMIT -1;",
      "SELECT id FROM users LIMIT;",
      "SELECT id FROM users LIMIT 1.5;",
      "SELECT DISTINCT DISTINCT id FROM users;",
  };
  for (const auto &sql : invalid) {
    auto result = parser.Parse(sql);
    if (!Check(!result.ok(), "非法 DISTINCT/LIMIT 应返回错误")) return false;
    if (!Check(result.status().message().find("位置") != std::string::npos,
               "DISTINCT/LIMIT 错误应包含位置")) {
      return false;
    }
  }

  return Check(
      oursql::ToString(parsed.value()[0]) ==
              "SelectStatement(table=users,projection=[name],"
              "distinct=true,limit=0)" &&
          oursql::ToString(plan.value()) ==
              "SelectPlan(table=users,root=ProjectPlan(columns=[name],"
              "child=SeqScanPlan(table=users)),distinct=true,limit=0)",
      "DISTINCT/LIMIT AST 和 Plan 文本格式应稳定");
}

bool TestDropTableAndAliasesCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;

  auto dropped = parser.Parse("DROP TABLE users;");
  if (!Check(dropped.ok(), "DROP TABLE 应解析成功")) return false;
  const auto &drop =
      std::get<oursql::DropTableStatement>(dropped.value()[0]);
  if (!Check(drop.table_name == "users" && drop.location.line == 1 &&
                 drop.location.column == 1,
             "DROP TABLE AST 字段应正确")) {
    return false;
  }
  auto drop_plan = planner.Build(dropped.value()[0], catalog);
  if (!Check(drop_plan.ok() &&
                 std::holds_alternative<oursql::DropTablePlan>(
                     drop_plan.value()),
             "DROP TABLE 应生成 DropTablePlan")) {
    return false;
  }
  if (!Check(oursql::ToString(drop_plan.value()) ==
                 "DropTablePlan(table=users)",
             "DROP TABLE Plan 文本应稳定")) {
    return false;
  }

  auto missing = parser.Parse("DROP TABLE missing;");
  auto missing_plan = planner.Build(missing.value()[0], catalog);
  if (!Check(missing.ok() && !missing_plan.ok() &&
                 missing_plan.status().code() == oursql::ErrorCode::NotFound,
             "DROP TABLE 不存在表应返回 NotFound")) {
    return false;
  }

  const std::vector<std::string> invalid_drop{
      "DROP users;",
      "DROP TABLE;",
      "DROP TABLE users",
  };
  for (const auto &sql : invalid_drop) {
    auto parsed = parser.Parse(sql);
    if (!Check(!parsed.ok(), "非法 DROP TABLE 应返回错误")) return false;
    if (!Check(parsed.status().message().find("位置") != std::string::npos,
               "DROP TABLE 错误应包含位置")) {
      return false;
    }
  }

  auto aliased = parser.Parse(
      "SELECT id AS user_id, name AS user_name "
      "FROM users AS u LIMIT 1;");
  if (!Check(aliased.ok(), "列别名和表别名应解析成功")) return false;
  const auto &select =
      std::get<oursql::SelectStatement>(aliased.value()[0]);
  if (!Check(select.table_alias == "u" &&
                 select.projection_aliases ==
                     std::vector<std::string>{"user_id", "user_name"} &&
                 select.limit.has_value() && *select.limit == 1,
             "AS 别名应保存到 SelectStatement")) {
    return false;
  }

  auto alias_plan = planner.Build(aliased.value()[0], catalog);
  if (!Check(alias_plan.ok() &&
                 std::holds_alternative<oursql::SelectPlan>(
                     alias_plan.value()),
             "AS 别名应生成 SelectPlan")) {
    return false;
  }
  const auto &select_plan = std::get<oursql::SelectPlan>(alias_plan.value());
  if (!Check(select_plan.table_alias == "u" &&
                 select_plan.projection_aliases ==
                     std::vector<std::string>{"user_id", "user_name"},
             "AS 别名应保留在 SelectPlan")) {
    return false;
  }
  if (!Check(
          oursql::ToString(aliased.value()[0]) ==
                  "SelectStatement(table=users,alias=u,projection=[id,name],"
                  "limit=1,aliases=[user_id,user_name])" &&
              oursql::ToString(alias_plan.value()) ==
                  "SelectPlan(table=users,root=ProjectPlan(columns=[id,name],"
                  "child=SeqScanPlan(table=users)),alias=u,limit=1,"
                  "aliases=[user_id,user_name])",
          "AS 别名 AST 和 Plan 文本格式应稳定")) {
    return false;
  }

  const std::vector<std::string> invalid_alias{
      "SELECT * AS all_columns FROM users;",
      "SELECT id AS FROM users;",
      "SELECT id FROM users AS;",
  };
  for (const auto &sql : invalid_alias) {
    auto parsed = parser.Parse(sql);
    if (!Check(!parsed.ok(), "非法 AS 别名应返回错误")) return false;
    if (!Check(parsed.status().message().find("位置") != std::string::npos,
               "AS 别名错误应包含位置")) {
      return false;
    }
  }
  return true;
}

bool TestCreateDropIndexAndExplain() {
  oursql::Parser parser;
  oursql::Planner planner;
  oursql::TableInfo table{
      "users",
      oursql::Schema{{"id", oursql::DataType::Int},
                     {"name", oursql::DataType::Varchar, 8}},
      oursql::INVALID_PAGE_ID};
  FixedCatalogReader no_indexes(table, {});
  FixedCatalogReader with_indexes(
      table, {oursql::IndexMetadata{
                 "idx_users_id", "users", "id", oursql::INVALID_PAGE_ID,
                 true}});

  auto create = parser.Parse("CREATE INDEX idx_users_id ON users(id);");
  if (!Check(create.ok(), "CREATE INDEX 应解析成功")) return false;
  const auto &create_statement =
      std::get<oursql::CreateIndexStatement>(create.value()[0]);
  if (!Check(create_statement.index_name == "idx_users_id" &&
                 create_statement.table_name == "users" &&
                 create_statement.column_name == "id" &&
                 !create_statement.is_unique,
             "CREATE INDEX AST 字段应正确")) {
    return false;
  }
  auto create_plan = planner.Build(create.value()[0], no_indexes);
  if (!Check(create_plan.ok() &&
                 std::holds_alternative<oursql::CreateIndexPlan>(
                     create_plan.value()),
             "CREATE INDEX 应生成 CreateIndexPlan")) {
    return false;
  }

  auto unique = parser.Parse(
      "CREATE UNIQUE INDEX idx_users_id ON users(id);");
  if (!Check(unique.ok(), "CREATE UNIQUE INDEX 应解析成功")) return false;
  const auto &unique_statement =
      std::get<oursql::CreateIndexStatement>(unique.value()[0]);
  if (!Check(unique_statement.is_unique,
             "CREATE UNIQUE INDEX 应保留 unique 标记")) {
    return false;
  }

  auto missing_column =
      parser.Parse("CREATE INDEX bad ON users(missing);");
  if (!Check(missing_column.ok(), "索引缺列样例应通过 Parser")) {
    return false;
  }
  auto missing_plan = planner.Build(missing_column.value()[0], no_indexes);
  if (!Check(!missing_plan.ok() &&
                 missing_plan.status().code() == oursql::ErrorCode::NotFound,
             "CREATE INDEX 应检查列是否存在")) {
    return false;
  }

  auto drop = parser.Parse("DROP INDEX idx_users_id;");
  if (!Check(drop.ok(), "DROP INDEX 应解析成功")) return false;
  const auto &drop_statement =
      std::get<oursql::DropIndexStatement>(drop.value()[0]);
  if (!Check(drop_statement.index_name == "idx_users_id",
             "DROP INDEX AST 字段应正确")) {
    return false;
  }
  auto drop_plan = planner.Build(drop.value()[0], no_indexes);
  if (!Check(drop_plan.ok() &&
                 std::holds_alternative<oursql::DropIndexPlan>(
                     drop_plan.value()),
             "DROP INDEX 应生成 DropIndexPlan")) {
    return false;
  }

  auto explain = parser.Parse(
      "EXPLAIN SELECT * FROM users WHERE id = 1;");
  if (!Check(explain.ok(), "EXPLAIN SELECT 应解析成功")) return false;
  auto seq_plan = planner.Build(explain.value()[0], no_indexes);
  if (!Check(seq_plan.ok() &&
                 std::holds_alternative<oursql::ExplainPlan>(
                     seq_plan.value()),
             "EXPLAIN 应生成 ExplainPlan")) {
    return false;
  }
  const auto &seq_explain = std::get<oursql::ExplainPlan>(seq_plan.value());
  const auto *seq_project =
      std::get_if<oursql::ProjectPlan>(&seq_explain.select.root->operation);
  const auto *seq_filter =
      seq_project && seq_project->child
          ? std::get_if<oursql::FilterPlan>(
                &seq_project->child->operation)
          : nullptr;
  if (!Check(seq_filter != nullptr &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     seq_filter->child->operation),
             "无索引 EXPLAIN 应显示 SeqScan")) {
    return false;
  }

  auto indexed_plan = planner.Build(explain.value()[0], with_indexes);
  if (!Check(indexed_plan.ok() &&
                 std::holds_alternative<oursql::ExplainPlan>(
                     indexed_plan.value()),
             "有索引 EXPLAIN 应规划成功")) {
    return false;
  }
  const auto &index_explain =
      std::get<oursql::ExplainPlan>(indexed_plan.value());
  const auto *index_project =
      std::get_if<oursql::ProjectPlan>(&index_explain.select.root->operation);
  const auto *index_filter =
      index_project && index_project->child
          ? std::get_if<oursql::FilterPlan>(
                &index_project->child->operation)
          : nullptr;
  if (!Check(index_filter != nullptr &&
                 std::holds_alternative<oursql::IndexScanPlan>(
                     index_filter->child->operation),
             "有索引 EXPLAIN 应显示 IndexScan")) {
    return false;
  }
  return Check(oursql::ToString(indexed_plan.value()).find(
                   "IndexScanPlan") != std::string::npos,
               "EXPLAIN 文本应包含 IndexScanPlan");
}

bool TestInnerJoinCompilePlan() {
  oursql::Catalog catalog;
  (void)catalog.CreateTable(oursql::TableInfo{
      "student",
      oursql::Schema{{"id", oursql::DataType::Int},
                     {"name", oursql::DataType::Varchar, 8},
                     {"common", oursql::DataType::Int}},
      oursql::INVALID_PAGE_ID});
  (void)catalog.CreateTable(oursql::TableInfo{
      "score",
      oursql::Schema{{"student_id", oursql::DataType::Int},
                     {"grade", oursql::DataType::Int},
                     {"common", oursql::DataType::Int}},
      oursql::INVALID_PAGE_ID});
  (void)catalog.CreateTable(oursql::TableInfo{
      "course",
      oursql::Schema{{"student_id", oursql::DataType::Int},
                     {"title", oursql::DataType::Varchar, 16}},
      oursql::INVALID_PAGE_ID});

  oursql::Parser parser;
  oursql::Planner planner;
  auto parsed = parser.Parse(
      "SELECT student.name, score.grade "
      "FROM student JOIN score ON student.id = score.student_id;");
  if (!Check(parsed.ok(), "两表 INNER JOIN 应解析成功")) return false;
  const auto &select =
      std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.joins.size() == 1 &&
                 select.joins[0].type == oursql::JoinType::Inner &&
                 select.joins[0].table_name == "score" &&
                 select.joins[0].condition.left_table == "student" &&
                 select.joins[0].condition.left_column == "id" &&
                 select.joins[0].condition.right_table == "score" &&
                 select.joins[0].condition.right_column == "student_id",
             "JOIN AST 字段应正确")) {
    return false;
  }

  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "INNER JOIN 应规划成功")) return false;
  const auto &select_plan = std::get<oursql::SelectPlan>(plan.value());
  const auto &project =
      std::get<oursql::ProjectPlan>(select_plan.root->operation);
  const auto *join =
      project.child ? std::get_if<oursql::JoinPlan>(&project.child->operation)
                    : nullptr;
  if (!Check(join != nullptr &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     join->left->operation) &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     join->right->operation) &&
                 project.columns ==
                     std::vector<std::string>{"name", "grade"} &&
                 project.input_indexes ==
                     std::vector<std::size_t>{1, 4} &&
                 join->left_column_index == std::optional<std::size_t>(0) &&
                 join->right_column_index == std::optional<std::size_t>(0),
             "JOIN Plan 与投影下标应正确")) {
    return false;
  }

  auto multi = parser.Parse(
      "SELECT student.name, score.grade, course.title "
      "FROM student "
      "JOIN score ON student.id = score.student_id "
      "JOIN course ON score.student_id = course.student_id;");
  if (!Check(multi.ok(), "多表 JOIN 应解析成功")) return false;
  const auto &multi_statement =
      std::get<oursql::SelectStatement>(multi.value()[0]);
  if (!Check(multi_statement.joins.size() == 2 &&
                 multi_statement.joins[0].table_name == "score" &&
                 multi_statement.joins[1].table_name == "course",
             "多表 JOIN AST 应保持子句顺序")) {
    return false;
  }
  auto multi_plan = planner.Build(multi.value()[0], catalog);
  if (!Check(multi_plan.ok(), "多表 JOIN 应规划成功")) return false;
  const auto &multi_select =
      std::get<oursql::SelectPlan>(multi_plan.value());
  const auto &multi_project =
      std::get<oursql::ProjectPlan>(multi_select.root->operation);
  const auto *outer_join =
      multi_project.child
          ? std::get_if<oursql::JoinPlan>(&multi_project.child->operation)
          : nullptr;
  const auto *inner_join =
      outer_join != nullptr && outer_join->left != nullptr
          ? std::get_if<oursql::JoinPlan>(&outer_join->left->operation)
          : nullptr;
  if (!Check(outer_join != nullptr && inner_join != nullptr &&
                 outer_join->join_type == oursql::JoinType::Inner &&
                 inner_join->join_type == oursql::JoinType::Inner &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     inner_join->left->operation) &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     inner_join->right->operation) &&
                 std::holds_alternative<oursql::SeqScanPlan>(
                     outer_join->right->operation) &&
                 multi_project.input_indexes ==
                     std::vector<std::size_t>{1, 4, 7},
             "多表 JOIN 应形成左深 JoinPlan 并正确映射投影")) {
    return false;
  }

  auto aliased = parser.Parse(
      "SELECT s.name, c.grade FROM student AS s "
      "INNER JOIN score AS c ON s.id = c.student_id;");
  if (!Check(aliased.ok(), "带别名 INNER JOIN 应解析成功")) return false;
  auto aliased_plan = planner.Build(aliased.value()[0], catalog);
  if (!Check(aliased_plan.ok() &&
                 std::holds_alternative<oursql::SelectPlan>(
                     aliased_plan.value()),
             "带别名 INNER JOIN 应规划成功")) {
    return false;
  }

  auto ambiguous = parser.Parse(
      "SELECT common FROM student JOIN score "
      "ON student.id = score.student_id;");
  if (!Check(ambiguous.ok(), "歧义列 JOIN 应先通过 Parser")) return false;
  auto ambiguous_plan = planner.Build(ambiguous.value()[0], catalog);
  if (!Check(!ambiguous_plan.ok() &&
                 ambiguous_plan.status().code() ==
                     oursql::ErrorCode::InvalidArgument,
             "同名列未限定时应报告歧义")) {
    return false;
  }

  const std::vector<std::pair<std::string, oursql::JoinType>> outer_joins{
      {"SELECT student.name FROM student LEFT JOIN score "
       "ON student.id = score.student_id;",
       oursql::JoinType::Left},
      {"SELECT student.name FROM student RIGHT OUTER JOIN score "
       "ON student.id = score.student_id;",
       oursql::JoinType::Right},
      {"SELECT student.name FROM student FULL JOIN score "
       "ON student.id = score.student_id;",
       oursql::JoinType::Full},
  };
  for (const auto &item : outer_joins) {
    auto parsed_outer = parser.Parse(item.first);
    if (!Check(parsed_outer.ok(), "外连接应可解析")) return false;
    const auto &outer_statement =
        std::get<oursql::SelectStatement>(parsed_outer.value()[0]);
    if (!Check(outer_statement.joins.size() == 1 &&
                   outer_statement.joins[0].type == item.second,
               "外连接 AST 类型应正确")) {
      return false;
    }
    auto outer_plan = planner.Build(parsed_outer.value()[0], catalog);
    if (!Check(outer_plan.ok(), "外连接应生成编译层 Plan")) return false;
    const auto &outer_select_plan =
        std::get<oursql::SelectPlan>(outer_plan.value());
    const auto &project_plan =
        std::get<oursql::ProjectPlan>(outer_select_plan.root->operation);
    const auto *join_plan =
        project_plan.child
            ? std::get_if<oursql::JoinPlan>(&project_plan.child->operation)
            : nullptr;
    if (!Check(join_plan != nullptr &&
                   join_plan->join_type == item.second,
               "外连接 JoinPlan 类型应正确")) {
      return false;
    }
  }
  return true;
}

bool TestSubqueryCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;

  auto in_query = parser.Parse(
      "SELECT id FROM users "
      "WHERE id NOT IN (SELECT id FROM users WHERE id = 1);");
  if (!Check(in_query.ok(), "IN 子查询应解析成功")) return false;
  const auto &in_select =
      std::get<oursql::SelectStatement>(in_query.value()[0]);
  const auto *in_expr =
      in_select.compile_where
          ? std::get_if<oursql::CompileSubqueryExpr>(
                &in_select.compile_where->data)
          : nullptr;
  if (!Check(in_expr != nullptr &&
                 in_expr->kind == oursql::CompileSubqueryKind::In &&
                 in_expr->negated && in_expr->query != nullptr &&
                 in_expr->query->table_name == "users",
             "IN 子查询 AST 应保存嵌套 SELECT 和否定标记")) {
    return false;
  }
  auto in_plan = planner.Build(in_query.value()[0], catalog);
  if (!Check(!in_plan.ok() &&
                 in_plan.status().code() ==
                     oursql::ErrorCode::NotImplemented,
             "IN 子查询应通过语义检查后明确报告执行未接入")) {
    return false;
  }

  auto exists_query = parser.Parse(
      "SELECT id FROM users "
      "WHERE EXISTS (SELECT id FROM users WHERE id = 1);");
  if (!Check(exists_query.ok(), "EXISTS 子查询应解析成功")) return false;
  const auto &exists_select =
      std::get<oursql::SelectStatement>(exists_query.value()[0]);
  const auto *exists_expr =
      exists_select.compile_where
          ? std::get_if<oursql::CompileSubqueryExpr>(
                &exists_select.compile_where->data)
          : nullptr;
  if (!Check(exists_expr != nullptr &&
                 exists_expr->kind ==
                     oursql::CompileSubqueryKind::Exists,
             "EXISTS 子查询 AST 应正确")) {
    return false;
  }
  auto exists_plan = planner.Build(exists_query.value()[0], catalog);
  if (!Check(!exists_plan.ok() &&
                 exists_plan.status().code() ==
                     oursql::ErrorCode::NotImplemented,
             "EXISTS 子查询应明确报告执行未接入")) {
    return false;
  }

  auto scalar_query = parser.Parse(
      "SELECT (SELECT id FROM users WHERE id = 1) FROM users;");
  if (!Check(scalar_query.ok() && scalar_query.value().size() == 1,
             "标量子查询应解析成功")) {
    return false;
  }
  const auto &scalar_select =
      std::get<oursql::SelectStatement>(scalar_query.value()[0]);
  const auto *scalar_expr =
      scalar_select.projection_expressions.size() == 1
          ? std::get_if<oursql::CompileSubqueryExpr>(
                &scalar_select.projection_expressions[0]->data)
          : nullptr;
  if (!Check(scalar_expr != nullptr &&
                 scalar_expr->kind ==
                     oursql::CompileSubqueryKind::Scalar,
             "标量子查询 AST 应正确")) {
    return false;
  }
  auto scalar_plan = planner.Build(scalar_query.value()[0], catalog);
  if (!Check(!scalar_plan.ok() &&
                 scalar_plan.status().code() ==
                     oursql::ErrorCode::NotImplemented,
             "SELECT 列表标量子查询应明确报告执行未接入")) {
    return false;
  }

  auto mismatch = parser.Parse(
      "SELECT id FROM users "
      "WHERE id IN (SELECT name FROM users);");
  if (!Check(mismatch.ok(), "类型不匹配子查询应先完成解析")) return false;
  auto mismatch_plan = planner.Build(mismatch.value()[0], catalog);
  if (!Check(!mismatch_plan.ok() &&
                 mismatch_plan.status().code() ==
                     oursql::ErrorCode::TypeMismatch,
             "IN 子查询两侧类型不一致应报告 TypeMismatch")) {
    return false;
  }

  auto multi_column = parser.Parse(
      "SELECT id FROM users "
      "WHERE id IN (SELECT id, name FROM users);");
  if (!Check(multi_column.ok(), "多列子查询应先完成解析")) return false;
  auto multi_column_plan = planner.Build(multi_column.value()[0], catalog);
  if (!Check(!multi_column_plan.ok() &&
                 multi_column_plan.status().code() ==
                     oursql::ErrorCode::InvalidArgument,
             "标量/IN 子查询返回多列应报告 InvalidArgument")) {
    return false;
  }

  auto missing_table = parser.Parse(
      "SELECT id FROM users "
      "WHERE id IN (SELECT id FROM ghost);");
  if (!Check(missing_table.ok(), "未知表子查询应先完成解析")) return false;
  auto missing_table_plan =
      planner.Build(missing_table.value()[0], catalog);
  return Check(!missing_table_plan.ok() &&
                   missing_table_plan.status().code() ==
                       oursql::ErrorCode::NotFound,
               "子查询中的未知表应报告 NotFound");
}

bool TestAlterTableCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  (void)catalog.CreateTable(oursql::TableInfo{
      "logs", oursql::Schema{{"id", oursql::DataType::Int}}});
  oursql::Parser parser;
  oursql::Planner planner;

  auto rename_table =
      parser.Parse("ALTER TABLE users RENAME TO accounts;");
  if (!Check(rename_table.ok(), "RENAME TO 应解析成功")) return false;
  const auto &rename_table_statement =
      std::get<oursql::AlterTableStatement>(rename_table.value()[0]);
  if (!Check(rename_table_statement.action ==
                     oursql::AlterTableAction::RenameTable &&
                 rename_table_statement.table_name == "users" &&
                 rename_table_statement.new_name == "accounts",
             "RENAME TO AST 字段应正确")) {
    return false;
  }
  auto rename_table_plan =
      planner.Build(rename_table.value()[0], catalog);
  if (!Check(rename_table_plan.ok() &&
                 std::holds_alternative<oursql::AlterTablePlan>(
                     rename_table_plan.value()),
             "RENAME TO 应生成 AlterTablePlan")) {
    return false;
  }

  auto rename_column = parser.Parse(
      "ALTER TABLE users RENAME COLUMN name TO display_name;");
  if (!Check(rename_column.ok(), "RENAME COLUMN 应解析成功")) return false;
  const auto &rename_column_statement =
      std::get<oursql::AlterTableStatement>(rename_column.value()[0]);
  if (!Check(rename_column_statement.action ==
                     oursql::AlterTableAction::RenameColumn &&
                 rename_column_statement.column_name == "name" &&
                 rename_column_statement.new_name == "display_name",
             "RENAME COLUMN AST 字段应正确")) {
    return false;
  }
  auto rename_column_plan =
      planner.Build(rename_column.value()[0], catalog);
  if (!Check(rename_column_plan.ok() &&
                 std::holds_alternative<oursql::AlterTablePlan>(
                     rename_column_plan.value()),
             "RENAME COLUMN 应生成 AlterTablePlan")) {
    return false;
  }

  auto add_column = parser.Parse(
      "ALTER TABLE users ADD COLUMN age INT DEFAULT 0;");
  if (!Check(add_column.ok(), "ADD COLUMN INT DEFAULT 应解析成功")) {
    return false;
  }
  const auto &add_column_statement =
      std::get<oursql::AlterTableStatement>(add_column.value()[0]);
  if (!Check(add_column_statement.action ==
                     oursql::AlterTableAction::AddColumn &&
                 add_column_statement.column.name == "age" &&
                 add_column_statement.column.type == oursql::DataType::Int &&
                 add_column_statement.default_value.has_value() &&
                 add_column_statement.default_value->AsInt() == 0,
             "ADD COLUMN AST 字段应正确")) {
    return false;
  }
  auto add_column_plan = planner.Build(add_column.value()[0], catalog);
  if (!Check(add_column_plan.ok() &&
                 std::holds_alternative<oursql::AlterTablePlan>(
                     add_column_plan.value()),
             "ADD COLUMN 应生成 AlterTablePlan")) {
    return false;
  }

  auto add_varchar = parser.Parse(
      "ALTER TABLE users ADD COLUMN city VARCHAR(16) DEFAULT 'Hong Kong';");
  if (!Check(add_varchar.ok(), "ADD COLUMN VARCHAR DEFAULT 应解析成功")) {
    return false;
  }
  auto add_varchar_plan = planner.Build(add_varchar.value()[0], catalog);
  if (!Check(add_varchar_plan.ok() &&
                 std::holds_alternative<oursql::AlterTablePlan>(
                     add_varchar_plan.value()),
             "ADD COLUMN VARCHAR 应生成 AlterTablePlan")) {
    return false;
  }

  auto missing_table =
      parser.Parse("ALTER TABLE ghost RENAME TO other;");
  auto missing_table_plan =
      planner.Build(missing_table.value()[0], catalog);
  if (!Check(!missing_table_plan.ok() &&
                 missing_table_plan.status().code() ==
                     oursql::ErrorCode::NotFound,
             "ALTER 未知表应报告 NotFound")) {
    return false;
  }

  auto existing_name =
      parser.Parse("ALTER TABLE users RENAME TO logs;");
  auto existing_name_plan =
      planner.Build(existing_name.value()[0], catalog);
  if (!Check(!existing_name_plan.ok() &&
                 existing_name_plan.status().code() ==
                     oursql::ErrorCode::AlreadyExists,
             "RENAME TO 已存在表应报告 AlreadyExists")) {
    return false;
  }

  auto missing_column = parser.Parse(
      "ALTER TABLE users RENAME COLUMN ghost TO other;");
  auto missing_column_plan =
      planner.Build(missing_column.value()[0], catalog);
  if (!Check(!missing_column_plan.ok() &&
                 missing_column_plan.status().code() ==
                     oursql::ErrorCode::NotFound,
             "RENAME COLUMN 旧列不存在应报告 NotFound")) {
    return false;
  }

  auto duplicate_column = parser.Parse(
      "ALTER TABLE users ADD COLUMN id INT DEFAULT 0;");
  auto duplicate_column_plan =
      planner.Build(duplicate_column.value()[0], catalog);
  if (!Check(!duplicate_column_plan.ok() &&
                 duplicate_column_plan.status().code() ==
                     oursql::ErrorCode::AlreadyExists,
             "ADD COLUMN 重复列应报告 AlreadyExists")) {
    return false;
  }

  auto bad_default = parser.Parse(
      "ALTER TABLE users ADD COLUMN age INT DEFAULT 'old';");
  auto bad_default_plan =
      planner.Build(bad_default.value()[0], catalog);
  return Check(!bad_default_plan.ok() &&
                   bad_default_plan.status().code() ==
                       oursql::ErrorCode::TypeMismatch,
               "ADD COLUMN DEFAULT 类型不匹配应报告 TypeMismatch");
}

bool TestColumnConstraintsCompilePlan() {
  oursql::Parser parser;
  oursql::Planner planner;
  auto parsed = parser.Parse(
      "CREATE TABLE users("
      "id INT PRIMARY KEY,"
      "email VARCHAR(32) UNIQUE,"
      "name VARCHAR(16) NOT NULL,"
      "age INT DEFAULT 18);");
  if (!Check(parsed.ok(), "列级约束应解析成功")) return false;
  const auto &create =
      std::get<oursql::CreateTableStatement>(parsed.value()[0]);
  if (!Check(create.schema.size() == 4 &&
                 create.schema.At(0).primary_key &&
                 !create.schema.At(0).nullable &&
                 create.schema.At(0).unique &&
                 create.schema.At(1).unique &&
                 !create.schema.At(2).nullable &&
                 create.schema.At(3).default_value.has_value() &&
                 create.schema.At(3).default_value->AsInt() == 18,
             "约束应保存到结构化 Column")) {
    return false;
  }

  oursql::Catalog empty;
  auto plan = planner.Build(parsed.value()[0], empty);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::CreateTablePlan>(
                     plan.value()),
             "合法约束应生成 CreateTablePlan")) {
    return false;
  }

  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"CREATE TABLE t(a INT PRIMARY KEY, b INT PRIMARY KEY);",
       oursql::ErrorCode::InvalidArgument},
      {"CREATE TABLE t(a INT NOT NULL DEFAULT NULL);",
       oursql::ErrorCode::InvalidArgument},
      {"CREATE TABLE t(a INT DEFAULT 'wrong');",
       oursql::ErrorCode::TypeMismatch},
      {"CREATE TABLE t(a VARCHAR(2) DEFAULT 'toolong');",
       oursql::ErrorCode::InvalidArgument},
  };
  for (const auto &item : invalid) {
    auto statements = parser.Parse(item.first);
    if (!Check(statements.ok(), "非法约束样例应先通过 Parser")) {
      return false;
    }
    auto invalid_plan = planner.Build(statements.value()[0], empty);
    if (!Check(!invalid_plan.ok() &&
                   invalid_plan.status().code() == item.second,
               "非法约束应返回预期错误码")) {
      return false;
    }
  }
  return true;
}

bool TestNullCompileAndPredicate() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;

  auto parsed = parser.Parse(
      "SELECT * FROM users WHERE name IS NULL;");
  if (!Check(parsed.ok(), "IS NULL 应解析成功")) return false;
  const auto &select =
      std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.where.has_value() &&
                 select.where->kind == oursql::PredicateKind::IsNull &&
                 select.where->column == "name",
             "IS NULL 应抽取为专用 Predicate")) {
    return false;
  }
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::SelectPlan>(plan.value()),
             "IS NULL 应生成可执行 SelectPlan")) {
    return false;
  }

  auto not_null = parser.Parse(
      "SELECT * FROM users WHERE name IS NOT NULL;");
  if (!Check(not_null.ok(), "IS NOT NULL 应解析成功")) return false;
  const auto &not_null_select =
      std::get<oursql::SelectStatement>(not_null.value()[0]);
  if (!Check(not_null_select.where.has_value() &&
                 not_null_select.where->kind ==
                     oursql::PredicateKind::IsNotNull,
             "IS NOT NULL 应抽取为专用 Predicate")) {
    return false;
  }

  auto equal_null = parser.Parse(
      "SELECT * FROM users WHERE name = NULL;");
  if (!Check(equal_null.ok(), "= NULL 应先完成语法解析")) return false;
  auto equal_null_plan = planner.Build(equal_null.value()[0], catalog);
  return Check(!equal_null_plan.ok() &&
                   equal_null_plan.status().code() ==
                       oursql::ErrorCode::InvalidArgument,
               "= NULL 应提示改用 IS NULL");
}

bool TestTransactionCompilePlan() {
  oursql::Parser parser;
  oursql::Planner planner;
  auto parsed = parser.Parse("BEGIN; COMMIT; ROLLBACK;");
  if (!Check(parsed.ok() && parsed.value().size() == 3,
             "事务控制多语句应解析成功")) {
    return false;
  }
  const std::vector<oursql::TransactionAction> expected{
      oursql::TransactionAction::Begin,
      oursql::TransactionAction::Commit,
      oursql::TransactionAction::Rollback,
  };
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto &statement =
        std::get<oursql::TransactionStatement>(parsed.value()[i]);
    if (!Check(statement.action == expected[i],
               "事务控制 AST action 应正确")) {
      return false;
    }
    auto plan = planner.Build(parsed.value()[i], oursql::Catalog{});
    if (!Check(plan.ok() &&
                   std::holds_alternative<oursql::TransactionPlan>(
                       plan.value()),
               "事务控制应生成 TransactionPlan")) {
      return false;
    }
    const auto &transaction = std::get<oursql::TransactionPlan>(plan.value());
    if (!Check(transaction.action == expected[i],
               "事务控制 Plan action 应正确")) {
      return false;
    }
  }
  if (!Check(oursql::ToString(parsed.value()[0]) ==
                 "TransactionStatement(action=begin)" &&
                 oursql::ToString(parsed.value()[1]) ==
                     "TransactionStatement(action=commit)" &&
                 oursql::ToString(parsed.value()[2]) ==
                     "TransactionStatement(action=rollback)",
             "事务 AST 文本格式应稳定")) {
    return false;
  }

  auto invalid = parser.Parse("BEGIN TRANSACTION;");
  return Check(!invalid.ok() &&
                   invalid.status().message().find("位置") !=
                       std::string::npos,
               "BEGIN TRANSACTION 当前应返回带位置的语法错误");
}

bool TestUpdateCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  auto parsed = parser.Parse(
      "UPDATE users SET name = 'Bob', id = 2 WHERE id = 1;");
  if (!Check(parsed.ok(), "UPDATE 应可解析")) return false;
  const auto &update =
      std::get<oursql::UpdateStatement>(parsed.value()[0]);
  if (!Check(update.table_name == "users" &&
                 update.assignments.size() == 2 &&
                 update.assignments[0].column == "name" &&
                 update.assignments[1].value.has_value() &&
                 update.assignments[1].value->AsInt() == 2 &&
                 update.where.has_value(),
             "UPDATE AST 核心字段应正确")) {
    return false;
  }
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok() &&
                 std::holds_alternative<oursql::UpdatePlan>(plan.value()),
             "UPDATE 应生成 UpdatePlan")) {
    return false;
  }

  auto missing_table = parser.Parse("UPDATE ghost SET id = 1;");
  auto bad_table = planner.Build(missing_table.value()[0], catalog);
  auto missing_column =
      parser.Parse("UPDATE users SET missing = 1;");
  auto bad_column = planner.Build(missing_column.value()[0], catalog);
  auto wrong_type =
      parser.Parse("UPDATE users SET id = 'text';");
  auto type_status = planner.Build(wrong_type.value()[0], catalog);
  auto expression_update =
      parser.Parse("UPDATE users SET id = id + 1;");
  if (!Check(expression_update.ok(), "UPDATE SET 表达式应可解析")) {
    return false;
  }
  const auto &expression_assignment =
      std::get<oursql::UpdateStatement>(expression_update.value()[0])
          .assignments[0];
  if (!Check(expression_assignment.expression != nullptr &&
                 !expression_assignment.value.has_value(),
             "UPDATE SET 表达式 AST 应保留 CompileExpr")) {
    return false;
  }
  auto expression_status =
      planner.Build(expression_update.value()[0], catalog);
  return Check(!bad_table.ok() &&
                   bad_table.status().code() == oursql::ErrorCode::NotFound &&
                   !bad_column.ok() &&
                   bad_column.status().code() == oursql::ErrorCode::NotFound &&
                   !type_status.ok() &&
                   type_status.status().code() == oursql::ErrorCode::TypeMismatch &&
                   expression_status.ok() &&
                   std::holds_alternative<oursql::UpdatePlan>(
                       expression_status.value()),
               "UPDATE 语义错误应被 Planner 拒绝");
}

bool TestOrderGroupCompileOnly() {
  oursql::Catalog catalog = MakeCatalog();
  oursql::Parser parser;
  oursql::Planner planner;
  const std::vector<std::string> accepted{
      "SELECT id FROM users ORDER BY id DESC;",
      "SELECT id FROM users GROUP BY id;",
      "SELECT id FROM users GROUP BY id ORDER BY id;",
  };
  for (const auto &sql : accepted) {
    auto parsed = parser.Parse(sql);
    if (!Check(parsed.ok(), "GROUP BY/ORDER BY 应可解析: " + sql)) return false;
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(plan.ok() &&
                   std::holds_alternative<oursql::SelectPlan>(plan.value()),
               "GROUP BY/ORDER BY 应生成 SelectPlan: " + sql)) {
      return false;
    }
    const auto &select = std::get<oursql::SelectPlan>(plan.value());
    const auto *project =
        std::get_if<oursql::ProjectPlan>(&select.root->operation);
    if (!Check(project != nullptr, "SELECT 根节点应为 ProjectPlan")) {
      return false;
    }
    if (sql.find("ORDER BY") != std::string::npos) {
      if (!Check(std::holds_alternative<oursql::OrderByPlan>(
                     project->child->operation),
                 "ORDER BY 应生成 OrderByPlan")) {
        return false;
      }
    } else if (sql.find("GROUP BY") != std::string::npos) {
      if (!Check(std::holds_alternative<oursql::GroupByPlan>(
                     project->child->operation),
                 "GROUP BY 应生成 GroupByPlan")) {
        return false;
      }
    }
  }
  // 列存在性仍然先于"未实现"被检查，保证诊断精确。
  auto missing = parser.Parse("SELECT id FROM users ORDER BY missing;");
  auto missing_plan = planner.Build(missing.value()[0], catalog);
  return Check(!missing_plan.ok() &&
                   missing_plan.status().code() == oursql::ErrorCode::NotFound,
               "ORDER BY 列不存在时应先报 NotFound");
}

bool TestCompileExpressionParsing() {
  oursql::Parser parser;
  auto parsed = parser.Parse(
      "SELECT * FROM users WHERE age > 1 AND (name = 'Alice' OR id >= 2);");
  if (!Check(parsed.ok(), "复杂 WHERE 表达式应可解析")) return false;
  const auto &select = std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.compile_where != nullptr && !select.where.has_value(),
             "复杂表达式不应伪装成数据库等值谓词")) {
    return false;
  }
  const std::string text = oursql::ToString(parsed.value()[0]);
  if (!Check(text.find("where_expr=") != std::string::npos &&
                 text.find(">") != std::string::npos &&
                 text.find("and") != std::string::npos,
             "复杂表达式应进入 AST 文本")) {
    return false;
  }

  auto catalog = MakeCatalog();
  oursql::Planner planner;
  auto missing = parser.Parse(
      "SELECT * FROM users WHERE missing > 1;");
  auto missing_status = planner.Build(missing.value()[0], catalog);
  auto wrong_type = parser.Parse(
      "SELECT * FROM users WHERE id + 'text' > 1;");
  auto type_status = planner.Build(wrong_type.value()[0], catalog);
  return Check(!missing_status.ok() &&
                   missing_status.status().code() ==
                       oursql::ErrorCode::NotFound &&
                   !type_status.ok() &&
                   type_status.status().code() ==
                       oursql::ErrorCode::TypeMismatch,
               "复杂表达式也应执行列和类型语义检查");
}

bool TestBetweenInLikeParsing() {
  oursql::Parser parser;
  oursql::Catalog catalog = MakeCatalog();
  oursql::Planner planner;

  auto between = parser.Parse(
      "SELECT * FROM users WHERE id BETWEEN 1 + 1 AND 2 + 1;");
  if (!Check(between.ok(), "BETWEEN AND 应解析成功")) return false;
  const auto &between_select =
      std::get<oursql::SelectStatement>(between.value()[0]);
  const auto *between_expr =
      between_select.compile_where
          ? std::get_if<oursql::CompileBetweenExpr>(
                &between_select.compile_where->data)
          : nullptr;
  const auto *between_lower =
      between_expr && between_expr->lower
          ? std::get_if<oursql::CompileLiteralExpr>(
                &between_expr->lower->data)
          : nullptr;
  const auto *between_upper =
      between_expr && between_expr->upper
          ? std::get_if<oursql::CompileLiteralExpr>(
                &between_expr->upper->data)
          : nullptr;
  if (!Check(between_expr != nullptr && !between_expr->negated &&
                 between_lower != nullptr &&
                 between_lower->integer == 2 &&
                 between_upper != nullptr &&
                 between_upper->integer == 3,
             "BETWEEN 的上下界应完成常量折叠")) {
    return false;
  }

  auto not_between = parser.Parse(
      "SELECT * FROM users WHERE id NOT BETWEEN 1 AND 3;");
  if (!Check(not_between.ok(), "NOT BETWEEN 应解析成功")) return false;
  const auto &not_between_select =
      std::get<oursql::SelectStatement>(not_between.value()[0]);
  const auto *not_between_expr =
      not_between_select.compile_where
          ? std::get_if<oursql::CompileBetweenExpr>(
                &not_between_select.compile_where->data)
          : nullptr;
  if (!Check(not_between_expr != nullptr && not_between_expr->negated,
             "NOT BETWEEN 应保留否定标记")) {
    return false;
  }

  auto in = parser.Parse(
      "SELECT * FROM users WHERE id NOT IN (1 + 1, 3, id);");
  if (!Check(in.ok(), "NOT IN 应解析成功")) return false;
  const auto &in_select =
      std::get<oursql::SelectStatement>(in.value()[0]);
  const auto *in_expr =
      in_select.compile_where
          ? std::get_if<oursql::CompileInExpr>(&in_select.compile_where->data)
          : nullptr;
  if (!Check(in_expr != nullptr && in_expr->negated &&
                 in_expr->options.size() == 3 &&
                 std::get_if<oursql::CompileColumnExpr>(
                     &in_expr->options[2]->data) != nullptr,
             "NOT IN 的列表表达式应被保留")) {
    return false;
  }

  auto like = parser.Parse(
      "SELECT * FROM users WHERE name LIKE 'A%';");
  if (!Check(like.ok(), "LIKE 应解析成功")) return false;
  const auto &like_select =
      std::get<oursql::SelectStatement>(like.value()[0]);
  const auto *like_expr =
      like_select.compile_where
          ? std::get_if<oursql::CompileBinaryExpr>(
                &like_select.compile_where->data)
          : nullptr;
  if (!Check(like_expr != nullptr && like_expr->op == "like",
             "LIKE 应表示为二元表达式")) {
    return false;
  }

  auto not_like = parser.Parse(
      "SELECT * FROM users WHERE name NOT LIKE 'A%';");
  if (!Check(not_like.ok(), "NOT LIKE 应解析成功")) return false;
  const auto &not_like_select =
      std::get<oursql::SelectStatement>(not_like.value()[0]);
  const auto *not_like_expr =
      not_like_select.compile_where
          ? std::get_if<oursql::CompileBinaryExpr>(
                &not_like_select.compile_where->data)
          : nullptr;
  if (!Check(not_like_expr != nullptr &&
                 not_like_expr->op == "not like",
             "NOT LIKE 应保留否定标记")) {
    return false;
  }

  auto constant_between = parser.Parse(
      "SELECT * FROM users WHERE 2 BETWEEN 1 + 1 AND 3;");
  if (!Check(constant_between.ok(), "常量 BETWEEN 应解析成功")) return false;
  const auto &constant_between_select =
      std::get<oursql::SelectStatement>(constant_between.value()[0]);
  const auto *constant_between_literal =
      constant_between_select.compile_where
          ? std::get_if<oursql::CompileLiteralExpr>(
                &constant_between_select.compile_where->data)
          : nullptr;
  if (!Check(constant_between_literal != nullptr &&
                 constant_between_literal->kind ==
                     oursql::CompileLiteralKind::Bool &&
                 constant_between_literal->text == "true",
             "纯常量 BETWEEN 应折叠为 TRUE")) {
    return false;
  }

  auto constant_in = parser.Parse(
      "SELECT * FROM users WHERE 2 IN (1 + 1, 3);");
  if (!Check(constant_in.ok(), "常量 IN 应解析成功")) return false;
  const auto &constant_in_select =
      std::get<oursql::SelectStatement>(constant_in.value()[0]);
  const auto *constant_in_literal =
      constant_in_select.compile_where
          ? std::get_if<oursql::CompileLiteralExpr>(
                &constant_in_select.compile_where->data)
          : nullptr;
  if (!Check(constant_in_literal != nullptr &&
                 constant_in_literal->kind ==
                     oursql::CompileLiteralKind::Bool &&
                 constant_in_literal->text == "true",
             "纯常量 IN 应折叠为 TRUE")) {
    return false;
  }

  const std::vector<std::string> valid{
      "SELECT * FROM users WHERE id BETWEEN 1 AND 3;",
      "SELECT * FROM users WHERE id IN (1, 2, 3);",
      "SELECT * FROM users WHERE name LIKE 'A%';",
  };
  for (const auto &sql : valid) {
    auto parsed = parser.Parse(sql);
    if (!Check(parsed.ok(), "合法 BETWEEN/IN/LIKE 应可解析")) return false;
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(!plan.ok() &&
                   plan.status().code() == oursql::ErrorCode::NotImplemented,
               "合法 BETWEEN/IN/LIKE 应完成语义检查后报告未实现")) {
      return false;
    }
  }

  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"SELECT * FROM users WHERE id BETWEEN 'a' AND 'b';",
       oursql::ErrorCode::TypeMismatch},
      {"SELECT * FROM users WHERE id IN (1, '2');",
       oursql::ErrorCode::TypeMismatch},
      {"SELECT * FROM users WHERE id LIKE '1%';",
       oursql::ErrorCode::TypeMismatch},
      {"SELECT * FROM users WHERE missing IN (1);",
       oursql::ErrorCode::NotFound},
  };
  for (const auto &item : invalid) {
    auto parsed = parser.Parse(item.first);
    if (!Check(parsed.ok(), "BETWEEN/IN/LIKE 语义样例应先通过 Parser")) {
      return false;
    }
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(!plan.ok() && plan.status().code() == item.second,
               "BETWEEN/IN/LIKE 应返回准确语义错误")) {
      return false;
    }
  }

  const std::vector<std::string> malformed{
      "SELECT * FROM users WHERE id IN ();",
      "SELECT * FROM users WHERE id BETWEEN 1 OR 2;",
      "SELECT * FROM users WHERE id BETWEEN 1 AND;",
      "SELECT * FROM users WHERE name LIKE;",
  };
  for (const auto &sql : malformed) {
    auto parsed = parser.Parse(sql);
    if (!Check(!parsed.ok(), "非法 BETWEEN/IN/LIKE 应返回语法错误")) {
      return false;
    }
    if (!Check(parsed.status().message().find("位置") != std::string::npos,
               "BETWEEN/IN/LIKE 语法错误应包含位置")) {
      return false;
    }
  }

  const std::string text = oursql::ToString(between.value()[0]);
  return Check(text.find("between") != std::string::npos &&
                   text.find(" and ") != std::string::npos,
               "BETWEEN 应进入稳定 AST 文本");
}

bool TestAggregateAndHavingCompileOnly() {
  oursql::Parser parser;
  oursql::Catalog catalog = MakeCatalog();
  oursql::Planner planner;

  const std::vector<std::pair<std::string, oursql::CompileAggregateKind>>
      aggregates{
          {"SELECT COUNT(*) FROM users;", oursql::CompileAggregateKind::Count},
          {"SELECT SUM(id) FROM users;", oursql::CompileAggregateKind::Sum},
          {"SELECT AVG(id) FROM users;", oursql::CompileAggregateKind::Avg},
          {"SELECT MAX(id) FROM users;", oursql::CompileAggregateKind::Max},
          {"SELECT MIN(id) FROM users;", oursql::CompileAggregateKind::Min},
      };
  for (const auto &item : aggregates) {
    auto parsed = parser.Parse(item.first);
    if (!Check(parsed.ok(), "聚合函数 SELECT 应解析成功")) return false;
    const auto &select =
        std::get<oursql::SelectStatement>(parsed.value()[0]);
    const auto *aggregate =
        select.projection_expressions.size() == 1
            ? std::get_if<oursql::CompileAggregateExpr>(
                  &select.projection_expressions[0]->data)
            : nullptr;
    if (!Check(aggregate != nullptr && aggregate->kind == item.second,
               "聚合函数 AST 类型应正确")) {
      return false;
    }
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(plan.ok() &&
                   std::get<oursql::SelectPlan>(plan.value()).has_aggregates,
               "聚合查询应生成带聚合标记的 SelectPlan")) {
      return false;
    }
  }

  auto grouped = parser.Parse(
      "SELECT name, COUNT(*) FROM users "
      "GROUP BY name HAVING COUNT(*) >= 2;");
  if (!Check(grouped.ok(), "GROUP BY + 聚合 + HAVING 应解析成功")) {
    return false;
  }
  const auto &grouped_select =
      std::get<oursql::SelectStatement>(grouped.value()[0]);
  const auto *having =
      grouped_select.having
          ? std::get_if<oursql::CompileBinaryExpr>(
                &grouped_select.having->data)
          : nullptr;
  if (!Check(grouped_select.projection_expressions.size() == 2 &&
                 having != nullptr && having->op == ">=",
             "HAVING 应保存完整聚合比较表达式")) {
    return false;
  }
  auto grouped_plan = planner.Build(grouped.value()[0], catalog);
  if (!Check(grouped_plan.ok(), "GROUP BY + HAVING 应规划成功")) {
    return false;
  }
  const auto &grouped_select_plan =
      std::get<oursql::SelectPlan>(grouped_plan.value());
  if (!Check(grouped_select_plan.has_aggregates &&
                 grouped_select_plan.having != nullptr &&
                 grouped_select_plan.projection_expressions.size() == 2,
             "聚合和 HAVING 应保留在 SelectPlan")) {
    return false;
  }
  const std::string grouped_text = oursql::ToString(grouped_plan.value());
  if (!Check(grouped_text.find("aggregates=true") != std::string::npos &&
                 grouped_text.find("having=") != std::string::npos,
             "Plan 文本应包含聚合和 HAVING 信息")) {
    return false;
  }

  const std::vector<std::pair<std::string, oursql::ErrorCode>> invalid{
      {"SELECT name, COUNT(*) FROM users;",
       oursql::ErrorCode::InvalidArgument},
      {"SELECT id FROM users GROUP BY name;",
       oursql::ErrorCode::InvalidArgument},
      {"SELECT COUNT(*) FROM users HAVING COUNT(*) > 0;",
       oursql::ErrorCode::InvalidArgument},
      {"SELECT SUM(name) FROM users;",
       oursql::ErrorCode::TypeMismatch},
      {"SELECT COUNT(*) FROM users WHERE COUNT(*) > 0;",
       oursql::ErrorCode::InvalidArgument},
      {"SELECT SUM(COUNT(*)) FROM users;",
       oursql::ErrorCode::InvalidArgument},
      {"SELECT SUM(*) FROM users;",
       oursql::ErrorCode::InvalidArgument},
  };
  for (const auto &item : invalid) {
    auto parsed = parser.Parse(item.first);
    if (!Check(parsed.ok(), "聚合语义错误样例应先通过 Parser")) {
      return false;
    }
    auto plan = planner.Build(parsed.value()[0], catalog);
    if (!Check(!plan.ok() && plan.status().code() == item.second,
               "聚合/HAVING 语义错误类型应准确")) {
      return false;
    }
  }
  return true;
}

bool TestCompileConstantFolding() {
  oursql::Parser parser;
  auto parsed = parser.Parse(
      "SELECT * FROM users WHERE id = 10 + 8;");
  if (!Check(parsed.ok(), "常量条件应可解析")) return false;
  const auto &select = std::get<oursql::SelectStatement>(parsed.value()[0]);
  if (!Check(select.where.has_value() &&
                 select.where->value.AsInt() == 18,
             "id = 10 + 8 应折叠为 Predicate(id,18)")) {
    return false;
  }

  auto true_condition = parser.Parse(
      "SELECT * FROM users WHERE 1 = 1;");
  if (!Check(true_condition.ok(), "1 = 1 应可解析")) return false;
  const auto &constant_select =
      std::get<oursql::SelectStatement>(true_condition.value()[0]);
  const auto *literal =
      constant_select.compile_where
          ? std::get_if<oursql::CompileLiteralExpr>(
                &constant_select.compile_where->data)
          : nullptr;
  if (!Check(literal != nullptr &&
                 literal->kind == oursql::CompileLiteralKind::Bool &&
                 literal->text == "true",
             "1 = 1 应折叠为 TRUE 字面量")) {
    return false;
  }

  auto catalog = MakeCatalog();
  oursql::Planner planner;
  auto plan = planner.Build(parsed.value()[0], catalog);
  if (!Check(plan.ok(), "折叠后的等值条件应可规划")) return false;
  const auto &select_plan = std::get<oursql::SelectPlan>(plan.value());
  const auto *project =
      std::get_if<oursql::ProjectPlan>(&select_plan.root->operation);
  const auto *filter =
      project == nullptr || project->child == nullptr
          ? nullptr
          : std::get_if<oursql::FilterPlan>(&project->child->operation);
  return Check(filter != nullptr &&
                   filter->predicate.value.AsInt() == 18,
               "折叠后的 Predicate 应进入 FilterPlan");
}

}  // namespace

int main() {
  int failures = 0;
  const auto run = [&](const char *name, bool (*test)()) {
    if (test()) std::cout << "[PASS] " << name << '\n';
    else ++failures;
  };
  run("Lexer tokens", &TestLexer);
  run("Lexer comments and number errors", &TestLexerCommentsAndNumberErrors);
  run("Parser AST and multi-statement", &TestParserAndCaseInsensitiveMultiStatement);
  run("Parser errors and positions", &TestParserErrorsHavePositions);
  run("Planner semantic checks", &TestPlannerSemanticChecks);
  run("Plan tree and printing", &TestPlanTreeAndPrinting);
  run("INSERT columns and multiple rows",
      &TestInsertColumnListAndMultipleRows);
  run("DISTINCT and LIMIT compile-only", &TestDistinctAndLimitCompileOnly);
  run("DROP TABLE and AS compile-only",
      &TestDropTableAndAliasesCompileOnly);
  run("CREATE/DROP INDEX and EXPLAIN", &TestCreateDropIndexAndExplain);
  run("INNER JOIN compile plan", &TestInnerJoinCompilePlan);
  run("Subquery compile-only", &TestSubqueryCompileOnly);
  run("ALTER TABLE compile-only", &TestAlterTableCompileOnly);
  run("Column constraints compile plan", &TestColumnConstraintsCompilePlan);
  run("NULL compile and predicate", &TestNullCompileAndPredicate);
  run("Transaction compile plan", &TestTransactionCompilePlan);
  run("UPDATE compile-only", &TestUpdateCompileOnly);
  run("ORDER BY/GROUP BY compile-only", &TestOrderGroupCompileOnly);
  run("Compile expression parsing", &TestCompileExpressionParsing);
  run("BETWEEN IN LIKE parsing", &TestBetweenInLikeParsing);
  run("Aggregate and HAVING compile-only",
      &TestAggregateAndHavingCompileOnly);
  run("Compile constant folding", &TestCompileConstantFolding);
  if (failures == 0) {
    std::cout << "All frontend tests passed\n";
    return 0;
  }
  std::cerr << failures << " frontend test(s) failed\n";
  return 1;
}
